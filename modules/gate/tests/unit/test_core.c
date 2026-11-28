/*
 * Gate data type: unit tests of the gate itself (gate_core.c).
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * HOW THE TESTS ARE WRITTEN. They drive the functions of gate_core.h with
 * explicit clocks and explicit draws and call gateCheckInvariants() after
 * every step. Unless a test says otherwise a gate has alpha 0.2, delta 0.2,
 * audit 0.25 and a feedback timeout of one second, so that everything
 * happens within a few dozen requests and proof waits a quarter of a second
 * (GRACE) for a late answer. A request is asked the way the module asks it:
 * maintain, evaluate for its draw (gateEvaluateFor(), which leaves out the
 * candidate's guess when the serving generation is going to answer),
 * predict. The library allocates through a counting allocator: every test
 * must give back each block it took.
 *
 * WHERE THE EXPECTED NUMBERS COME FROM. None was produced by gate_core.c.
 *
 * The bar of generation g is log(g (g + 1)) + log(tested rows * 32
 * thresholds) - log(delta): every threshold is a test of its own. With
 * delta 0.2 the wealth a region must reach is
 *
 *     score gate, generation 1, 2, 3:              320,  960, 1920
 *     two tested labels, generation 1, 2, 3:       640, 1920, 3840
 *     three tested labels, generation 1, 2, 3:     960, 2880, 5760
 *
 * A region that has seen n correct answers and k wrong ones holds
 *
 *     wealth(n, k) = 1/4 sum_j (1 + c_j alpha / (1 - alpha))^n (1 - c_j)^k
 *
 * with c = 0.1, 0.25, 0.5, 0.9 (gate_stats.h). The smallest n that reaches
 * a target was computed with exact rational arithmetic:
 *
 *     alpha 0.2, target 320:  k = 0: 35   k = 1: 47   k = 2: 57   k = 3: 68
 *     alpha 0.2, target 640:  k = 0: 39   k = 1: 50   k = 2: 61   k = 3: 72
 *                             k = 4: 82   k = 5: 92
 *     alpha 0.2, target 960:  k = 0: 41   k = 1: 52
 *     alpha 0.2, target 1920: k = 0: 44   k = 1: 56
 *     alpha 0.2, target 2880: 46     target 3840: 48     target 5760: 50
 *     alpha 0.2, target 20480: 56
 *     alpha 0.1, target 640: 82     target 960: 87
 *
 * The first one by hand: for alpha 0.2 a correct answer multiplies the four
 * bets by 1.025, 1.0625, 1.125 and 1.225. After 35 of them they hold 2.4,
 * 8.4, 61.7 and 1215.6: a quarter of the sum is 322. After 34 it is 264.
 * logWealth() below evaluates the same formula in floating point and the
 * first test requires that it reproduces this table.
 *
 * THE BUDGET of a classifier snapshot is 10 times the k = 0 column (the
 * proof a flawless threshold needs) for its most demanding tested label,
 * times the tested labels: 780, 880 and 960 for generation 1, 2 and 3 of
 * two labels, 1230, 1380 and 1500 of three.
 *
 * THE DRIFT DETECTOR (SPEC.md section 5, gate_stats.h) numbers the spot
 * checks of a row 1, 2, 3, ... as they are ASKED and cuts them into blocks
 * of 64, of 512 and of 4096. For alpha 0.2 its two alternatives are error
 * rates of 0.4 and 0.6, so a block in which n spot checks were asked and w
 * of them are known to disagree holds
 *
 *     stat(n, w) = (2^w 0.75^(n - w) + 3^w 0.5^(n - w)) / 2
 *
 * (for alpha 0.1 and below: 2 and 4 for a disagreement). A spot check counts
 * as right until its answer says otherwise, whenever that answer arrives.
 * The alarm "got worse" is raised by the disagreement that takes a block to
 * 1000: the 7th in a fresh block ((128 + 2187) / 2 = 1157.5; six give
 * 396.5), the 8th when one more spot check was asked and is right or still
 * unanswered (stat(8, 7) = 594.75, stat(9, 8) = 1736.25), the 9th with two
 * such, the 11th after six right ones, the 13th after ten. The second
 * statistic of a block counts lost answers as disagreements too and raises
 * "cannot be checked" at 10^9: 20 in a fresh block ((2^20 + 3^20) / 2 =
 * 1.74e9; 19 give 5.8e8), the 24th of 31 asked at once, the 25th of 32.
 * driftStat() below is that formula; exact values come from the scratch
 * script named in the report (rational arithmetic).
 *
 * LATE ANSWERS. When proof has moved on without an answer, the request was
 * counted as a wrong guess; when the answer arrives after all and the guess
 * was right, the gate adds (gain of a correct answer - loss of a wrong one)
 * to each log wealth. The counters then are exactly those of an answer that
 * came in its turn. The wealth is the same sum added up in another order:
 * the tests allow it the rounding of that many additions (wealthTol()),
 * 1e-12 for the few dozen answers most of them use.
 *
 * The randomized tests at the end compare the gate, operation by operation,
 * with a second implementation of the rules of gate_core.h that is written
 * for obviousness instead of speed (one array, linear scans), and with a
 * second gate that is told only what a replica is told. The second
 * implementation also keeps, for every region, the wealth the true answers
 * give it: the gate's must never be above that.
 *
 * Usage: test_core [seed ...]. Without arguments every test runs. With
 * seeds, only the three randomized walks run, once for each seed, and say
 * where they went; "score:seed", "classifier:seed" or "takeovers:seed"
 * runs one of the three.
 *
 * WITH THE CODEC. The Makefile links this program without gate_codec.c.
 * Built by hand with -DTEST_CORE_CODEC and gate_codec.c, every inv() also
 * encodes the gate, decodes the bytes, requires that the copy is the gate
 * and encodes to the same bytes, and the replica of the randomized walks is
 * replaced now and then by a gate decoded from the primary's bytes. That is
 * how "whatever the core did with a gate, it can be saved and loaded" is
 * checked here; the command line is in the report.
 */

#include "testutil.h"
#include "gate_core.h"
#ifdef TEST_CORE_CODEC
#include "gate_codec.h"
#endif

#define T GATE_THRESHOLDS
#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

#define ALPHA 0.2
#define DELTA 0.2
#define AUDIT 0.25
#define TIMEOUT 1000 /* feedback timeout, ms */
#define GRACE 250    /* a quarter of it: how long proof waits for a late answer */
#define T0 1000000   /* creation time, ms */
#define LINEAGE 0x1111aaaa2222bbbbull
#define EPOCH 0x0123456789abcdefull
#define EPOCH2 0x0fedcba987654321ull
#define EPOCH3 0x00c0ffee00c0ffeeull
#define U_SPOT 0.1   /* a draw below AUDIT: a spot check */
#define U_PLAIN 0.9  /* a draw that is not */

/* Clue vectors: one bucket per label. The value 4 makes a model confident
 * after a handful of examples. */
static uint32_t xa_idx[] = {3}, xb_idx[] = {700}, xc_idx[] = {8191};
static float x_val[] = {4.0f};
static const gateSparse XA = {1, xa_idx, x_val};
static const gateSparse XB = {1, xb_idx, x_val};
static const gateSparse XC = {1, xc_idx, x_val};

/* ------------------------------- Allocator -------------------------------
 * Counts the blocks the library holds, and can make an allocation fail. */

static long live_blocks = 0; /* blocks the library holds right now */
static long fail_in = -1;    /* allocations left before one fails; -1: never */
static int fail_forever = 0; /* once one failed, the following fail too */

static int allocFails(void) {
    if (fail_in < 0) return 0;
    if (fail_in > 0) {
        fail_in--;
        return 0;
    }
    if (!fail_forever) fail_in = -1;
    return 1;
}

static void *testMalloc(size_t n) {
    void *p = allocFails() ? NULL : malloc(n);

    if (p) live_blocks++;
    return p;
}

static void *testCalloc(size_t nmemb, size_t size) {
    void *p = allocFails() ? NULL : calloc(nmemb, size);

    if (p) live_blocks++;
    return p;
}

static void *testRealloc(void *old, size_t n) {
    void *p = allocFails() ? NULL : realloc(old, n);

    if (p && old == NULL) live_blocks++;
    return p;
}

static void testFree(void *p) {
    live_blocks--;
    free(p);
}

/* Run a test and require that it returned every block. */
#define RUN_LEAKFREE(name) do { \
    RUN(name); \
    gate_test_checks++; \
    if (live_blocks != 0) { \
        gate_test_failures++; \
        printf("  %s: %ld blocks were not released\n", #name, live_blocks); \
        live_blocks = 0; \
    } \
} while (0)

/* -------------------------------- Helpers -------------------------------- */

static void die(const char *why) {
    printf("%s: %s\n", gate_test_current, why);
    exit(2);
}

/* 0 while a test holds counters beyond GATE_COUNT_MAX: the encoding writes
 * those as the limit, so what is decoded is not what was encoded. */
static int codec_exact = 1;

#ifdef TEST_CORE_CODEC
static uint64_t fingerprint(const gate *g, int skip);
static uint64_t codec_trips = 0, codec_calls = 0;

/* The gate as bytes and back (see WITH THE CODEC in the file comment):
 * whatever the core made of a gate must encode, decode to the same gate,
 * and encode to the same bytes again. */
static void roundTrip(const gate *g) {
    unsigned char *a = NULL, *b = NULL;
    size_t alen = 0, blen = 0;
    char err[160] = "";
    gate *copy;

    if (fail_in >= 0) return; /* an allocation failure is being injected */
    /* With thousands of requests waiting, on a sample of the calls. */
    if (g->npending > 256 && (++codec_calls & 63)) return;
    gate_test_checks++;
    codec_trips++;
    if (gateEncode(g, 0, &a, &alen) != GATE_OK) die("gateEncode failed");
    copy = gateDecode(a, alen, err, sizeof(err));
    if (copy == NULL) {
        gate_test_failures++;
        printf("  in %s: the gate cannot be decoded from its own encoding: %s\n",
               gate_test_current, err);
        gate_free(a);
        return;
    }
    if (gateEncode(copy, 0, &b, &blen) != GATE_OK) die("gateEncode failed");
    if (alen != blen || memcmp(a, b, alen) != 0) {
        gate_test_failures++;
        printf("  in %s: the decoded gate encodes to other bytes\n", gate_test_current);
    }
    if (codec_exact && fingerprint(copy, 0) != fingerprint(g, 0)) {
        gate_test_failures++;
        printf("  in %s: the decoded gate is not the gate that was encoded\n", gate_test_current);
    }
    if (!gateCheckInvariants(copy, err, sizeof(err))) {
        gate_test_failures++;
        printf("  in %s: decoded gate: invariant broken: %s\n", gate_test_current, err);
    }
    gateFree(copy);
    gate_free(a);
    gate_free(b);
}
#else
static void roundTrip(const gate *g) { (void)g; }
#endif

static void inv(const gate *g) {
    char err[160] = "";

    gate_test_checks++;
    if (!gateCheckInvariants(g, err, sizeof(err))) {
        gate_test_failures++;
        printf("  in %s: invariant broken: %s\n", gate_test_current, err);
    }
    roundTrip(g);
}

/* The serving / candidate generation, or a blank stand-in when the gate
 * has none (number 0, every policy -99, no proof): a test that expects a
 * generation then fails its checks instead of crashing the whole run. */
static gateRegion blank_regions[GATE_MAX_CLASSES * T];
static int32_t blank_policy[GATE_MAX_CLASSES]; /* set in main() */
static gateDrift blank_drift[GATE_MAX_CLASSES];
static uint64_t blank_paired[GATE_MAX_CLASSES * T];
static const gateGeneration blank = {
    .regions = blank_regions, .policy = blank_policy, .drift = blank_drift, .paired = blank_paired
};

static const gateGeneration *servingOf(const gate *g) {
    return g->serving ? g->serving : &blank;
}

static const gateGeneration *candidateOf(const gate *g) {
    return g->candidate ? g->candidate : &blank;
}

/* Log of the wealth a region of generation 'gen' must reach: every one of
 * the 32 thresholds of every tested row is a test of its own. */
static double barOf(uint64_t gen, uint32_t tested_rows, double delta) {
    return log((double)gen * ((double)gen + 1.0)) + log((double)tested_rows * 32.0) - log(delta);
}

/* Log of the wealth of a region after 'n' correct and 'k' wrong answers:
 * the formula of the file comment. */
static double logWealth(double alpha, uint64_t n, uint64_t k) {
    static const double c[4] = {0.1, 0.25, 0.5, 0.9};
    double wealth = 0.0;
    int j;

    for (j = 0; j < 4; j++)
        wealth += pow(1.0 + c[j] * alpha / (1.0 - alpha), (double)n) * pow(1.0 - c[j], (double)k);
    return log(wealth / 4.0);
}

/* What the log wealth of a region that has seen 'n' answers may differ by
 * from logWealth(): each log wealth is a running sum of n increments of at
 * most 2.31 (the log of 0.1), so each addition rounds by at most
 * n * 2.31 * 2^-53. That is 3e-16 n^2 for the sum; 1e-12 covers the
 * formula itself. */
static double wealthTol(uint64_t n) {
    return 1e-12 + 3e-16 * (double)n * (double)n;
}

/* Correct answers a region needs to reach 'bar' when it has also seen 'k'
 * wrong ones before getting there. */
static uint32_t needCorrect(double alpha, double bar, uint32_t k) {
    uint32_t n;

    for (n = 1;; n++)
        if (logWealth(alpha, n, k) >= bar) return n;
}

/* Number of thresholds at or below 'score': the regions it belongs to. */
static uint32_t regionsOf(const gate *g, double score) {
    uint32_t t, n = 0;

    for (t = 0; t < T; t++)
        if (g->grid[t] <= score) n++;
    return n;
}

/* Log of the statistic of a block of the drift detector in which 'n' spot
 * checks were asked and 'w' of them count against the gate: stat(n, w) of
 * the file comment, for any alpha (SPEC.md section 5: likelihood ratios for
 * 2x and 4x the error rate, at most halfway to 1, averaged). */
static double driftStat(double alpha, uint64_t n, uint64_t w) {
    double cap = 0.5 * (1.0 + alpha), l[2], hi, lo;
    int k;

    /* The log of each likelihood ratio, then the log of their average: a
     * block of thousands of spot checks holds numbers like 3^2000. */
    for (k = 0; k < 2; k++) {
        double p1 = (k ? 4.0 : 2.0) * alpha;

        if (p1 > cap) p1 = cap;
        l[k] = (double)w * log(p1 / alpha) + (double)(n - w) * log((1.0 - p1) / (1.0 - alpha));
    }
    hi = l[0] > l[1] ? l[0] : l[1];
    lo = l[0] > l[1] ? l[1] : l[0];
    return hi + log((1.0 + exp(lo - hi)) / 2.0);
}

/* Spot checks until the alarm "got worse" when they are asked one after
 * the other and 'pattern' says what becomes of each: 'x' its answer
 * disagrees and arrives at once, '.' it agrees, 'o' it has no answer yet.
 * 0 if the alarm is not raised: the detector of gate_stats.h by itself. */
static uint32_t alarmAfter(double alpha, const char *pattern) {
    gateDriftBets bets;
    gateDrift d;
    uint32_t i;

    gateDriftBetsInit(&bets, alpha);
    gateDriftInit(&d);
    for (i = 0; pattern[i]; i++) {
        uint64_t number = gateDriftAsk(&d);

        if (pattern[i] == 'x' &&
            gateDriftWrong(&d, &bets, number, log(GATE_DRIFT_ALARM), log(GATE_LOST_ALARM)) ==
                GATE_DRIFT_ALARM_WORSE)
            return i + 1;
    }
    return 0;
}

/* The same for lost answers and the alarm "cannot be checked": every spot
 * check of 'asked' is asked first, then their answers are given up in the
 * order they were asked. Which one raises it; 0 if none does. */
static uint32_t lostAlarmAfter(double alpha, uint32_t asked) {
    gateDriftBets bets;
    gateDrift d;
    uint32_t i;

    gateDriftBetsInit(&bets, alpha);
    gateDriftInit(&d);
    for (i = 0; i < asked; i++) gateDriftAsk(&d);
    for (i = 1; i <= asked; i++)
        if (gateDriftLost(&d, &bets, i, log(GATE_LOST_ALARM)) == GATE_DRIFT_ALARM_UNCHECKED)
            return i;
    return 0;
}

static void baseConfig(gateConfig *cfg) {
    gateConfigInit(cfg);
    cfg->alpha = ALPHA;
    cfg->delta = DELTA;
    cfg->audit = AUDIT;
    cfg->feedback_timeout_ms = TIMEOUT;
}

/* A classifier config with labels "label0", "label1", ... */
static void classConfig(gateConfig *cfg, uint32_t k, uint32_t snapshot_examples) {
    char names[GATE_MAX_CLASSES + 1][12];
    const char *ptrs[GATE_MAX_CLASSES + 1];
    size_t lens[GATE_MAX_CLASSES + 1];
    uint32_t i;

    baseConfig(cfg);
    cfg->kind = GATE_KIND_CLASSIFIER;
    cfg->snapshot_examples = snapshot_examples;
    for (i = 0; i < k; i++) {
        lens[i] = (size_t)snprintf(names[i], sizeof(names[i]), "label%u", i);
        ptrs[i] = names[i];
    }
    if (gateConfigSetClasses(cfg, ptrs, lens, k) != GATE_OK) die("out of memory");
}

/* Validate, create, and release the config: the gate keeps its own copy. */
static gate *create(gateConfig *cfg) {
    char err[128];
    gate *g;

    if (gateConfigValidate(cfg, err, sizeof(err)) != GATE_OK) die(err);
    g = gateCreate(cfg, LINEAGE, EPOCH, T0);
    if (g == NULL) die("gateCreate failed");
    gateConfigFree(cfg);
    inv(g);
    return g;
}

static gate *scoreGate(void) {
    gateConfig cfg;

    baseConfig(&cfg);
    return create(&cfg);
}

static gate *classGate(uint32_t k, uint32_t snapshot_examples) {
    gateConfig cfg;

    classConfig(&cfg, k, snapshot_examples);
    return create(&cfg);
}

/* GPREDICT on a score gate, the way the module does it: maintain, evaluate
 * for the draw, predict. */
static gatePredictResult ask(gate *g, int64_t now, double u, double score) {
    gatePredictResult res;
    gateEval ev;

    gateMaintain(g, now);
    CHECK(gateEvaluateFor(g, NULL, score, u, &ev) == GATE_OK);
    CHECK(gatePredict(g, now, u, &ev, NULL, &res) == GATE_OK);
    inv(g);
    return res;
}

/* Guesses chosen by hand, for the generations the gate has right now. */
static gateEval guesses(const gate *g, int32_t serv_label, double serv_score,
                        int32_t cand_label, double cand_score)
{
    gateEval ev;

    memset(&ev, 0, sizeof(ev));
    ev.serving.label = ev.candidate.label = -1;
    if (g->serving) {
        ev.has_serving = 1;
        ev.serving.label = serv_label;
        ev.serving.score = serv_score;
    }
    if (g->candidate) {
        ev.has_candidate = 1;
        ev.candidate.label = cand_label;
        ev.candidate.score = cand_score;
    }
    return ev;
}

/* GPREDICT on a classifier gate with given guesses and clues. */
static gatePredictResult askGuess(gate *g, int64_t now, double u, const gateEval *ev,
                                  const gateSparse *x)
{
    gatePredictResult res;
    gateSparse copy;

    if (gateSparseCopy(&copy, x) != GATE_OK) die("out of memory");
    CHECK(gatePredict(g, now, u, ev, &copy, &res) == GATE_OK);
    gateSparseFree(&copy); /* still ours unless the request was recorded */
    inv(g);
    return res;
}

/* GPREDICT key TEXT on a classifier gate. */
static gatePredictResult askText(gate *g, int64_t now, double u, const gateSparse *x) {
    gateEval ev;

    gateMaintain(g, now);
    CHECK(gateEvaluateFor(g, x, 0.0, u, &ev) == GATE_OK);
    return askGuess(g, now, u, &ev, x);
}

/* GLEARN. */
static int answer(gate *g, int64_t now, uint64_t seq, int32_t result) {
    int used = gateLearn(g, now, g->epoch, seq, result);

    inv(g);
    return used;
}

/* A request that must be recorded, and its answer. */
static void feed(gate *g, int64_t now, double u, double score, int correct) {
    gatePredictResult r = ask(g, now, u, score);

    CHECK(r.seq != 0);
    CHECK_EQ_INT(answer(g, now, r.seq, correct), 1);
}

static void feedText(gate *g, int64_t now, double u, const gateSparse *x, int32_t label) {
    gatePredictResult r = askText(g, now, u, x);

    CHECK(r.seq != 0);
    CHECK_EQ_INT(answer(g, now, r.seq, label), 1);
}

/* GADD. */
static void train(gate *g, int64_t now, const gateSparse *x, int32_t label) {
    CHECK(gateTrain(g, now, x, label) == GATE_OK);
    inv(g);
}

/* A fresh score gate brought to ACTIVE: correct answers at 0.9 prove the
 * thresholds 0..11 together, so it answers everything. */
static gate *activeScoreGate(int64_t now) {
    gate *g = scoreGate();
    uint32_t i, n = needCorrect(ALPHA, barOf(1, 1, DELTA), 0);

    for (i = 0; i < n; i++) feed(g, now, U_PLAIN, 0.9, 1);
    CHECK(g->serving != NULL && servingOf(g)->policy[0] == 0);
    return g;
}

/* A two-label gate whose first snapshot has just been taken after 'n'
 * examples, XA -> label 0 and XB -> label 1 in turn. */
static gate *trainedGate(uint32_t n) {
    gate *g = classGate(2, n);
    uint32_t i;

    for (i = 0; i < n; i++) train(g, T0, (i & 1) ? &XB : &XA, (int32_t)(i & 1));
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 1);
    return g;
}

/* Flags of a waiting request, all ones if it is not waiting. */
static uint32_t flagsOf(const gate *g, uint64_t seq) {
    const gateObs *o = gateFindObservation(g, seq);

    return o ? o->flags : ~0u;
}

/* A two-label gate by made-up guesses: generation 1 serves (label0 is
 * answered from the lowest threshold, 0.5; label1 has no proven threshold)
 * and generation 2 is under test, with no proof yet. Two tested labels: the
 * wealth to reach is 640 for generation 1 (39 correct answers) and 1920 for
 * generation 2 (44, or 56 after one wrong answer). */
static gate *twoGenerations(int64_t now) {
    gate *g = classGate(2, 5);
    gatePredictResult r;
    gateEval ev;
    uint32_t i;

    for (i = 0; i < 5; i++) train(g, now, &XA, 0);
    for (i = 0; i < 39; i++) {
        ev = guesses(g, -1, 0.0, 0, 0.99);
        r = askGuess(g, now, U_PLAIN, &ev, &XA);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
    }
    CHECK(g->serving != NULL && servingOf(g)->gen == 1 && servingOf(g)->samples == 39);
    CHECK(servingOf(g)->policy[0] == 0 && servingOf(g)->policy[1] == -1);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 2 && candidateOf(g)->samples == 0);
    return g;
}

/* The thresholds lo..hi-1 of a row have seen 'n' answers, 'k' of them
 * counted as wrong, and hold the wealth that goes with that. */
static void expectRegions(const gate *g, const gateGeneration *gen, uint32_t row, uint32_t lo,
                          uint32_t hi, uint64_t n, uint64_t k)
{
    uint32_t t;

    for (t = lo; t < hi; t++) {
        const gateRegion *reg = &gen->regions[row * T + t];

        CHECK_EQ_INT(reg->n, n);
        CHECK_EQ_INT(reg->errors, k);
        CHECK_NEAR(gateRegionLogWealth(reg), logWealth(gateRowAlpha(g, row), n - k, k),
                   wealthTol(n));
    }
}

/* What the serving generation guessed for 'n' requests. */
typedef struct servGuess {
    int32_t label;
    double score;
    uint64_t n;
} servGuess;

/* The paired counts of a generation, entry by entry, by their definition:
 * for every row and every threshold, how many of the listed requests the
 * serving generation's own guess put in that row at or above that
 * threshold. A label that is never answered is counted nowhere. */
static void expectPaired(const gate *g, const gateGeneration *gen, const servGuess *guess,
                         size_t nguess)
{
    uint32_t r, t;
    size_t i;

    for (r = 0; r < g->nrows; r++) {
        for (t = 0; t < T; t++) {
            uint64_t n = 0;

            for (i = 0; i < nguess; i++)
                if ((uint32_t)guess[i].label == r && gateRowTested(g, r) &&
                    g->grid[t] <= guess[i].score)
                    n += guess[i].n;
            CHECK_EQ_INT(gen->paired[r * T + t], n);
        }
    }
}

/* A drift detector in whose first block of 64 everything so far happened
 * (the blocks of 512 and of 4096 then hold the same): spot checks asked,
 * disagreeing answers, and of the open block the disagreements and the
 * answers given up. */
static void expectDrift(const gateDrift *d, uint64_t asked, uint64_t errors, uint32_t wrong,
                        uint32_t lost)
{
    int s;

    CHECK(asked <= 64);
    CHECK_EQ_INT(d->asked, asked);
    CHECK_EQ_INT(d->errors, errors);
    for (s = 0; s < GATE_DRIFT_SCALES; s++) {
        CHECK_EQ_INT(d->wrong[s], wrong);
        CHECK_EQ_INT(d->lost[s], lost);
        CHECK_EQ_INT(gateDriftOpen(d, s), asked);
    }
}

/* The spot check number of a waiting request, all ones if it is not
 * waiting. */
static uint64_t spotOf(const gate *g, uint64_t seq) {
    const gateObs *o = gateFindObservation(g, seq);

    return o ? o->spot : ~(uint64_t)0;
}

/* ------------------------------ Fingerprints -----------------------------
 * A hash of everything a gate holds, to state "nothing changed" and "these
 * two gates are in the same state" in one comparison. */

#define SKIP_CLOCK 1    /* now_ms */
#define SKIP_REQUESTS 2 /* the request counters only a primary keeps */
#define SKIP_WEIGHTS 4  /* model weights (slow to hash) */

static uint64_t mix(uint64_t h, uint64_t v) {
    h = (h ^ v) * 0x9e3779b97f4a7c15ull;
    return h ^ (h >> 29);
}

static uint64_t mixDouble(uint64_t h, double v) {
    uint64_t bits;

    memcpy(&bits, &v, sizeof(bits));
    return mix(h, bits);
}

static uint64_t mixFloats(uint64_t h, const float *v, size_t n) {
    size_t i;

    for (i = 0; i < n; i++) {
        uint32_t bits;

        memcpy(&bits, &v[i], sizeof(bits));
        h = mix(h, bits);
    }
    return h;
}

static uint64_t mixModel(uint64_t h, const gateModel *m, int skip) {
    h = mix(h, m->updates);
    h = mix(h, m->w != NULL);
    if (m->w == NULL || (skip & SKIP_WEIGHTS)) return h;
    h = mixFloats(h, m->w, (size_t)m->nvec * m->dim);
    return mixFloats(h, m->b, m->nvec);
}

static uint64_t mixGeneration(uint64_t h, const gate *g, const gateGeneration *gen, int skip) {
    uint32_t i, r;
    int j;

    if (gen == NULL) return mix(h, 0);
    h = mix(h, gen->gen);
    h = mix(h, (uint64_t)gen->frozen_ms);
    h = mix(h, (uint64_t)gen->promoted_ms);
    h = mixDouble(h, gen->log_threshold);
    h = mix(h, gen->samples);
    h = mix(h, gen->errors);
    h = mix(h, gen->audits);
    h = mix(h, gen->audit_errors);
    h = mix(h, gen->missed);
    h = mix(h, gen->budget);
    for (i = 0; i < g->nrows * T; i++) {
        const gateRegion *reg = &gen->regions[i];

        for (j = 0; j < GATE_NBETS; j++) h = mixDouble(h, reg->logw[j]);
        h = mix(h, reg->n);
        h = mix(h, reg->errors);
        h = mix(h, reg->certified_at);
        h = mix(h, gen->paired[i]);
    }
    for (r = 0; r < g->nrows; r++) {
        const gateDrift *d = &gen->drift[r];

        h = mix(h, (uint64_t)(int64_t)gen->policy[r]);
        h = mix(h, d->asked);
        h = mix(h, d->errors);
        for (j = 0; j < GATE_DRIFT_SCALES; j++) {
            h = mix(h, d->wrong[j]);
            h = mix(h, d->lost[j]);
        }
    }
    return mixModel(h, &gen->model, skip);
}

static uint64_t fingerprint(const gate *g, int skip) {
    uint64_t h = 0x243f6a8885a308d3ull;
    const gateObs *o;
    uint32_t i;

    h = mix(h, g->lineage);
    h = mix(h, g->epoch);
    h = mix(h, (uint64_t)g->created_ms);
    if (!(skip & SKIP_CLOCK)) h = mix(h, (uint64_t)g->now_ms);
    h = mix(h, g->generations);
    h = mix(h, g->snapshot_updates);
    h = mix(h, (uint64_t)g->paused);
    h = mix(h, g->missing_run);
    h = mix(h, g->answer_mark);
    h = mix(h, (uint64_t)g->blind);
    h = mix(h, g->next_seq);
    h = mix(h, g->npending);
    h = mix(h, g->nanswered);
    h = mix(h, g->nevidence);
    h = mix(h, g->pending_bytes);
    h = mixModel(h, &g->trainer, skip);
    h = mixGeneration(h, g, g->candidate, skip);
    h = mixGeneration(h, g, g->serving, skip);
    for (o = g->oldest; o; o = o->next) {
        h = mix(h, o->seq);
        h = mix(h, (uint64_t)o->created_ms);
        h = mix(h, o->flags);
        h = mix(h, (uint64_t)(int64_t)o->cand_label);
        h = mix(h, (uint64_t)(int64_t)o->serv_label);
        h = mixDouble(h, o->cand_score);
        h = mixDouble(h, o->serv_score);
        h = mix(h, o->cand_gen);
        h = mix(h, o->serv_gen);
        h = mix(h, (uint64_t)(int64_t)o->result);
        h = mix(h, o->spot);
        h = mix(h, o->x.n);
        for (i = 0; i < o->x.n; i++) h = mix(h, o->x.idx[i]);
        h = mixFloats(h, o->x.val, o->x.n);
    }
    for (o = g->ehead; o; o = o->enext) h = mix(h, o->seq);
    if (!(skip & SKIP_REQUESTS)) {
        h = mix(h, g->stats.requests);
        h = mix(h, g->stats.accepted);
        h = mix(h, g->stats.audited);
        h = mix(h, g->stats.abstained);
        h = mix(h, g->stats.full);
        h = mix(h, g->stats.window);
        h = mixDouble(h, g->stats.coverage);
    }
    h = mix(h, g->stats.observations);
    h = mix(h, g->stats.learned);
    h = mix(h, g->stats.ignored);
    h = mix(h, g->stats.missing);
    h = mix(h, g->stats.dropped);
    h = mix(h, g->stats.promotions);
    h = mix(h, g->stats.revocations);
    return mix(h, g->stats.retired);
}

/* Maintenance at 'now' is not due, and does nothing but move the clock. */
static void expectIdle(gate *g, int64_t now) {
    uint64_t before = fingerprint(g, SKIP_CLOCK);

    CHECK_EQ_INT(gateMaintenanceDue(g, now), 0);
    gateMaintain(g, now);
    inv(g);
    CHECK(fingerprint(g, SKIP_CLOCK) == before);
}

/* Maintenance at 'now' is due, and does something. */
static void expectDue(gate *g, int64_t now) {
    uint64_t before = fingerprint(g, SKIP_CLOCK);

    CHECK_EQ_INT(gateMaintenanceDue(g, now), 1);
    CHECK(fingerprint(g, SKIP_CLOCK) == before); /* asking changes nothing */
    gateMaintain(g, now);
    inv(g);
    CHECK(fingerprint(g, SKIP_CLOCK) != before);
    CHECK_EQ_INT(gateMaintenanceDue(g, now), 0);
}

/* Bytes of a gate, added up from what it is made of: the struct, the
 * labels with their two per-label settings, the per-row increments, the
 * request index, the models (one weight vector for two labels, else one per
 * label, each with its bias), the generations and the waiting requests. */
static uint64_t expectBytes(const gate *g) {
    const gateGeneration *gens[2];
    uint64_t n = sizeof(gate), model = 0;
    const gateObs *o;
    uint32_t i, k = g->cfg.nclasses;

    gens[0] = g->candidate;
    gens[1] = g->serving;
    for (i = 0; i < k; i++)
        n += sizeof(char *) + strlen(g->cfg.labels[i]) + 1 + sizeof(double) + sizeof(uint8_t);
    n += (uint64_t)g->nrows * (sizeof(gateBets) + sizeof(gateDriftBets));
    n += (uint64_t)GATE_TABLE_SIZE * sizeof(gateObs *);
    if (k) model = (uint64_t)(k == 2 ? 1 : k) * (GATE_DIM + 1) * sizeof(float);
    n += model;
    for (i = 0; i < 2; i++) {
        if (gens[i] == NULL) continue;
        n += sizeof(gateGeneration) + model;
        n += (uint64_t)g->nrows * T * sizeof(gateRegion);
        n += (uint64_t)g->nrows * T * sizeof(uint64_t); /* the paired counts */
        n += (uint64_t)g->nrows * (sizeof(int32_t) + sizeof(gateDrift));
    }
    for (o = g->oldest; o; o = o->next)
        n += sizeof(gateObs) + (uint64_t)o->x.n * (sizeof(uint32_t) + sizeof(float));
    return n;
}

/* ---------------------------- Expected numbers --------------------------- */

TEST(expected_numbers) {
    static const struct {
        double alpha, target;
        uint32_t k, need;
    } table[] = {
        {0.2, 320.0, 0, 35},  {0.2, 320.0, 1, 47},  {0.2, 320.0, 2, 57},  {0.2, 320.0, 3, 68},
        {0.2, 640.0, 0, 39},  {0.2, 640.0, 1, 50},  {0.2, 640.0, 2, 61},  {0.2, 640.0, 3, 72},
        {0.2, 640.0, 4, 82},  {0.2, 640.0, 5, 92},
        {0.2, 960.0, 0, 41},  {0.2, 960.0, 1, 52},  {0.2, 1920.0, 0, 44}, {0.2, 1920.0, 1, 56},
        {0.2, 2880.0, 0, 46}, {0.2, 3840.0, 0, 48}, {0.2, 5760.0, 0, 50}, {0.2, 20480.0, 0, 56},
        {0.1, 640.0, 0, 82}
    };
    /* Blocks of the drift detector: spot checks asked, of which count
     * against the gate, and the statistic (file comment; alpha 0.2). */
    static const struct {
        uint64_t n, w;
        double stat;
    } blocks[] = {
        {1, 1, 2.5},       {2, 1, 1.5},        {6, 6, 396.5},     {7, 7, 1157.5},
        {7, 6, 206.25},    {8, 7, 594.75},     {9, 8, 1736.25},   {11, 8, 464.0625},
        {12, 9, 1338.1875}, {6, 2, 0.9140625}, {8, 8, 3408.5},    {20, 20, 1743916488.5},
        {19, 19, 581392877.5}
    };
    size_t i;

    CHECK_NEAR(barOf(1, 1, 0.2), log(320.0), 1e-12);
    CHECK_NEAR(barOf(3, 1, 0.2), log(1920.0), 1e-12);
    CHECK_NEAR(barOf(1, 2, 0.2), log(640.0), 1e-12);
    CHECK_NEAR(barOf(2, 2, 0.2), log(1920.0), 1e-12);
    CHECK_NEAR(barOf(3, 3, 0.2), log(5760.0), 1e-12);
    /* The wealth of the file comment: 322 after 35 correct answers, 264
     * after 34; one wrong answer among 36 leaves 40.2. */
    CHECK_NEAR(exp(logWealth(0.2, 35, 0)), 321.988, 0.001);
    CHECK_NEAR(exp(logWealth(0.2, 34, 0)), 264.321, 0.001);
    CHECK_NEAR(exp(logWealth(0.2, 35, 1)), 40.200, 0.001);
    CHECK(logWealth(0.2, 0, 0) == 0.0);
    for (i = 0; i < COUNTOF(table); i++) {
        gateBets bets;
        gateRegion reg;
        uint32_t n;

        CHECK_EQ_INT(needCorrect(table[i].alpha, log(table[i].target), table[i].k),
                     table[i].need);
        /* The betting code the gate is built on draws the line at the same
         * answer: wrong ones first, then one correct answer short, then the
         * last. */
        gateBetsInit(&bets, table[i].alpha);
        gateRegionInit(&reg);
        for (n = 0; n < table[i].k; n++) gateRegionUpdate(&reg, &bets, 1);
        for (n = 1; n < table[i].need; n++) gateRegionUpdate(&reg, &bets, 0);
        CHECK(!gateRegionCertifies(&reg, log(table[i].target)));
        gateRegionUpdate(&reg, &bets, 0);
        CHECK(gateRegionCertifies(&reg, log(table[i].target)));
        /* The k = 0 column is the proof a flawless threshold needs: what
         * the budget of a snapshot is made of. */
        if (table[i].k == 0)
            CHECK_EQ_INT(gateBetsFlawless(&bets, log(table[i].target)), table[i].need);
    }

    /* The drift detector: the formula against the values of the file
     * comment, and the detector against the formula. */
    for (i = 0; i < COUNTOF(blocks); i++) {
        gateDriftBets bets;
        gateDrift d;
        uint64_t j;

        CHECK_NEAR(exp(driftStat(0.2, blocks[i].n, blocks[i].w)) / blocks[i].stat, 1.0, 1e-12);
        gateDriftBetsInit(&bets, 0.2);
        gateDriftInit(&d);
        for (j = 1; j <= blocks[i].n; j++) CHECK_EQ_INT(gateDriftAsk(&d), j);
        /* The last 'w' of them disagree, whatever comes of that. */
        for (j = blocks[i].n; j > blocks[i].n - blocks[i].w; j--)
            (void)gateDriftWrong(&d, &bets, j, INFINITY, INFINITY);
        CHECK_NEAR(gateDriftLogStat(&d, &bets, 0), log(blocks[i].stat), 1e-12);
        CHECK_NEAR(gateDriftLogStat(&d, &bets, 1), log(blocks[i].stat), 1e-12);
        CHECK_EQ_INT(d.errors, blocks[i].w);
    }
    /* Only disagreements, from a fresh block: the 7th (1157.5, six give
     * 396.5). For an alpha of 0.1 and below a disagreement counts 2 and 4:
     * the 6th ((64 + 4096) / 2 = 2080, five give 528). */
    CHECK_EQ_INT(alarmAfter(0.2, "xxxxxxxxxx"), 7);
    CHECK_EQ_INT(alarmAfter(0.1, "xxxxxxxxxx"), 6);
    CHECK_EQ_INT(alarmAfter(0.05, "xxxxxxxxxx"), 6);
    CHECK_EQ_INT(alarmAfter(0.2, "xxxxxx"), 0);
    CHECK_EQ_INT(alarmAfter(0.05, "xxxxx"), 0);
    /* Four disagree, three agree, then five more disagree: stat(12, 9) =
     * 1338.2, where stat(11, 8) = 464.1. */
    CHECK_EQ_INT(alarmAfter(0.2, "xxxx...xxxxxxxx"), 12);
    /* stat(8, 7) = 594.75, stat(9, 8) = 1736.25. */
    CHECK_EQ_INT(alarmAfter(0.2, "xxxxxx.xxx"), 9);
    /* No restart inside a block: spot checks that agreed first are still
     * in it. After six of them it takes eleven (stat(17, 11) = 1566,
     * stat(16, 10) = 552), where a fresh block takes seven. A spot check
     * without an answer counts like one that agreed. */
    CHECK_EQ_INT(alarmAfter(0.2, "......xxxxxxxxxxxxx"), 17);
    CHECK_EQ_INT(alarmAfter(0.2, "ooooooxxxxxxxxxxxxx"), 17);
    CHECK_EQ_INT(alarmAfter(0.2, "oxxxxxxxxx"), 9);
    CHECK_EQ_INT(alarmAfter(0.2, "ooxxxxxxxxxxx"), 11);
    /* ... and it can take until the next block of 64: after 57 that agreed,
     * the 7 that fill the block do not get it to 1000 (128 * 0.75^57 is
     * 1e-5), and the block of 512 would need 35. The 7th of the next block
     * of 64 does: the 14th disagreement, spot check 71. */
    {
        char pattern[79];

        memset(pattern, '.', 57);
        memset(pattern + 57, 'x', 21);
        pattern[78] = '\0';
        CHECK_EQ_INT(alarmAfter(0.2, pattern), 71);
        CHECK(driftStat(0.2, 64, 7) < 0.0 && driftStat(0.2, 70, 13) < 0.0);
    }
    /* Lost answers: 20 in a fresh block; of 31 asked at once the 24th
     * (stat(31, 24) = 1.10e9, stat(31, 23) = 1.8e8), of 32 the 25th
     * (stat(32, 25) = 3.3e9, stat(32, 24) = 5.5e8). Small alphas: the 16th
     * ((2^16 + 4^16) / 2 = 2.1e9; 15 give 5.4e8). */
    CHECK_EQ_INT(lostAlarmAfter(0.2, 20), 20);
    CHECK_EQ_INT(lostAlarmAfter(0.2, 19), 0);
    CHECK_EQ_INT(lostAlarmAfter(0.2, 31), 24);
    CHECK_EQ_INT(lostAlarmAfter(0.2, 32), 25);
    CHECK_EQ_INT(lostAlarmAfter(0.1, 16), 16);
    CHECK_EQ_INT(lostAlarmAfter(0.05, 16), 16);
    CHECK_EQ_INT(lostAlarmAfter(0.05, 15), 0);
    CHECK(driftStat(0.2, 31, 24) >= log(1e9) && driftStat(0.2, 31, 23) < log(1e9));
    CHECK(driftStat(0.2, 32, 25) >= log(1e9) && driftStat(0.2, 32, 24) < log(1e9));
    /* A flip is the difference of the two increments: after (wrong, right,
     * right, flip) a region holds what three right answers give it. */
    {
        gateBets bets;
        gateRegion late, prompt;

        gateBetsInit(&bets, 0.2);
        gateRegionInit(&late);
        gateRegionInit(&prompt);
        gateRegionUpdate(&late, &bets, 1);
        gateRegionUpdate(&late, &bets, 0);
        gateRegionUpdate(&late, &bets, 0);
        gateRegionFlip(&late, &bets);
        for (i = 0; i < 3; i++) gateRegionUpdate(&prompt, &bets, 0);
        CHECK(late.n == 3 && late.errors == 0 && prompt.n == 3 && prompt.errors == 0);
        CHECK_NEAR(gateRegionLogWealth(&late), gateRegionLogWealth(&prompt), 1e-14);
        CHECK_NEAR(gateRegionLogWealth(&late), logWealth(0.2, 3, 0), 1e-14);
    }
}

/* --------------------------------- Names --------------------------------- */

TEST(names) {
    CHECK_EQ_STR(gateStateName(GATE_STATE_COLD), "COLD");
    CHECK_EQ_STR(gateStateName(GATE_STATE_CALIBRATING), "CALIBRATING");
    CHECK_EQ_STR(gateStateName(GATE_STATE_ACTIVE), "ACTIVE");
    CHECK_EQ_STR(gateStateName(GATE_STATE_PAUSED), "PAUSED");
    CHECK_EQ_STR(gateActionName(GATE_ACTION_ACCEPT), "ACCEPT");
    CHECK_EQ_STR(gateActionName(GATE_ACTION_ABSTAIN), "ABSTAIN");
    CHECK_EQ_STR(gateActionName(GATE_ACTION_AUDIT), "AUDIT");
    CHECK(gateStateName((gateStateId)77) != NULL);
    CHECK(gateActionName(77) != NULL);
}

/* ----------------------------- Configuration ----------------------------- */

TEST(config_defaults) {
    gateConfig cfg;
    char err[128] = "";

    memset(&cfg, 0xff, sizeof(cfg));
    gateConfigInit(&cfg);
    CHECK_EQ_INT(cfg.kind, GATE_KIND_SCORE);
    CHECK(cfg.alpha == 0.0);
    CHECK(cfg.delta == 0.01); /* SPEC.md: DELTA */
    CHECK(cfg.audit == 0.05); /* SPEC.md: AUDIT */
    CHECK_EQ_INT(cfg.nclasses, 0);
    CHECK(cfg.labels == NULL && cfg.class_alpha == NULL && cfg.class_defer == NULL);
    CHECK_EQ_INT(cfg.snapshot_examples, 1000);
    CHECK_EQ_INT(cfg.feedback_timeout_ms, 60 * 1000); /* one minute */

    /* ALPHA has no default: it is the one thing the user must say. */
    CHECK(gateConfigValidate(&cfg, err, sizeof(err)) == GATE_ERR);
    CHECK(err[0] != '\0');
    cfg.alpha = 0.1;
    CHECK(gateConfigValidate(&cfg, err, sizeof(err)) == GATE_OK);
    gateConfigFree(&cfg);

    /* The fixed choices of SPEC.md section 5 (and CHANGES.md) that the
     * tests below are written around. */
    CHECK_EQ_INT(GATE_THRESHOLDS, 32);
    CHECK_EQ_INT(GATE_GRACE_DIVISOR, 4);    /* proof waits a quarter of the timeout */
    CHECK_EQ_INT(GATE_MAX_PENDING, 4096);   /* waiting requests */
    CHECK_EQ_INT(GATE_TRAIN_PENDING, 3072); /* ... of which may be training-only */
    CHECK_EQ_INT(GATE_MAX_WAITING, 16384);  /* answers waiting for an older answer */
    CHECK_EQ_INT(GATE_MISSING_RUN, 32);     /* answers missing in a row */
    CHECK(GATE_DRIFT_ALARM == 1000.0);
    CHECK(GATE_LOST_ALARM == 1e9);          /* ... with lost answers counted in */
    CHECK_EQ_INT(GATE_DRIFT_SCALES, 3);
    CHECK(gateDriftBlocks[0] == 64 && gateDriftBlocks[1] == 512 && gateDriftBlocks[2] == 4096);
    CHECK_EQ_INT(GATE_CANDIDATE_BUDGET_FACTOR, 10);
    CHECK(GATE_COUNT_MAX == (uint64_t)1 << 62); /* no counter or number goes beyond */
    CHECK(GATE_MAX_CLOCK_MS == INT64_MAX - 7ll * 86400 * 1000);
    CHECK_EQ_INT(GATE_MAX_SNAPSHOT_EXAMPLES, 1000000000);
    CHECK_EQ_INT(TIMEOUT / GATE_GRACE_DIVISOR, GRACE);
}

TEST(config_classes) {
    /* Labels are (pointer, length) pairs: nothing after the length counts. */
    static const char *const labels[] = {"spamXXXX", "ham", "caf\xc3\xa9"};
    static const size_t lens[] = {4, 3, 5};
    gateConfig cfg;
    char err[128];
    uint32_t i;

    gateConfigInit(&cfg);
    cfg.kind = GATE_KIND_CLASSIFIER;
    CHECK(gateConfigSetClasses(&cfg, labels, lens, 3) == GATE_OK);
    CHECK_EQ_INT(cfg.nclasses, 3);
    CHECK_EQ_STR(cfg.labels[0], "spam");
    CHECK_EQ_STR(cfg.labels[1], "ham");
    CHECK_EQ_STR(cfg.labels[2], "caf\xc3\xa9");
    CHECK(cfg.labels[1] != labels[1]); /* copied */
    for (i = 0; i < 3; i++) CHECK(cfg.class_defer[i] == 0);

    /* The command may name ALPHA after CLASSES: a label nobody gave a rate
     * of its own gets the gate's rate when the config is validated. */
    cfg.alpha = 0.3;
    CHECK(gateConfigValidate(&cfg, err, sizeof(err)) == GATE_OK);
    for (i = 0; i < 3; i++) CHECK(cfg.class_alpha[i] == 0.3);

    CHECK_EQ_INT(gateConfigLabelIndex(&cfg, "spam", 4), 0);
    CHECK_EQ_INT(gateConfigLabelIndex(&cfg, "hamster", 3), 1);
    CHECK_EQ_INT(gateConfigLabelIndex(&cfg, "caf\xc3\xa9", 5), 2);
    CHECK_EQ_INT(gateConfigLabelIndex(&cfg, "ha", 2), -1);
    CHECK_EQ_INT(gateConfigLabelIndex(&cfg, "hamster", 7), -1);
    CHECK_EQ_INT(gateConfigLabelIndex(&cfg, "Ham", 3), -1);
    CHECK_EQ_INT(gateConfigLabelIndex(&cfg, "", 0), -1);

    gateConfigFree(&cfg);
    CHECK(cfg.labels == NULL && cfg.class_alpha == NULL && cfg.class_defer == NULL);
    CHECK_EQ_INT(cfg.nclasses, 0);
    CHECK(cfg.alpha == 0.0);
}

/* Labels are kept as C strings. One with a zero byte inside would be cut
 * short there and then pass for a valid, different label: refused, wherever
 * the byte is, and nothing is taken. */
TEST(config_label_with_zero_byte) {
    static const char *const labels[] = {"spam", "ha\0m", "eggs"};
    static const struct {
        size_t lens[3];
        int ok;
    } cases[] = {
        {{4, 4, 4}, 0}, /* "ha\0m": in the middle */
        {{4, 3, 4}, 0}, /* "ha\0": at the end */
        {{5, 2, 4}, 0}, /* "spam\0": the terminator counted in */
        {{4, 2, 5}, 0}, /* ... in the last label */
        {{4, 2, 4}, 1}  /* "spam", "ha", "eggs" */
    };
    static const char *const nul[] = {"\0", "x"};
    static const size_t nul_lens[] = {1, 1};
    gateConfig cfg;
    char err[128];
    size_t i;

    for (i = 0; i < COUNTOF(cases); i++) {
        gateConfigInit(&cfg);
        cfg.kind = GATE_KIND_CLASSIFIER;
        cfg.alpha = ALPHA;
        CHECK_EQ_INT(gateConfigSetClasses(&cfg, labels, cases[i].lens, 3),
                     cases[i].ok ? GATE_OK : GATE_ERR);
        if (cases[i].ok) {
            CHECK_EQ_INT(cfg.nclasses, 3);
            CHECK_EQ_STR(cfg.labels[1], "ha");
            CHECK(gateConfigValidate(&cfg, err, sizeof(err)) == GATE_OK);
            CHECK_EQ_INT(gateConfigLabelIndex(&cfg, "ha", 2), 1);
            CHECK_EQ_INT(gateConfigLabelIndex(&cfg, "ha\0m", 4), -1);
        } else {
            CHECK(cfg.labels == NULL && cfg.class_alpha == NULL && cfg.class_defer == NULL);
            CHECK_EQ_INT(cfg.nclasses, 0);
            /* The config is as it was: it can still be given its classes. */
            CHECK(gateConfigSetClasses(&cfg, labels, cases[COUNTOF(cases) - 1].lens, 3) == GATE_OK);
        }
        gateConfigFree(&cfg);
    }
    gateConfigInit(&cfg);
    cfg.kind = GATE_KIND_CLASSIFIER;
    CHECK(gateConfigSetClasses(&cfg, nul, nul_lens, 2) == GATE_ERR); /* nothing but the byte */
    CHECK(cfg.labels == NULL && cfg.nclasses == 0);
    gateConfigFree(&cfg);
}

static void relabel(gateConfig *cfg, uint32_t i, const char *s) {
    gate_free(cfg->labels[i]);
    cfg->labels[i] = gate_malloc(strlen(s) + 1);
    if (cfg->labels[i] == NULL) die("out of memory");
    strcpy(cfg->labels[i], s);
}

static void expectVerdict(gateConfig *cfg, int verdict, const char *what) {
    char err[128];
    int got;

    memset(err, 0, sizeof(err));
    got = gateConfigValidate(cfg, err, sizeof(err));
    gate_test_checks++;
    if (got != verdict || (verdict == GATE_ERR && err[0] == '\0')) {
        gate_test_failures++;
        printf("  in %s: %s: %s\n", gate_test_current, what,
               verdict == GATE_ERR ? "accepted, or refused without a message" : err);
    }
    gateConfigFree(cfg);
}

/* Start from a valid three-label config, change one thing, validate. */
#define REFUSED(change) do { \
    gateConfig c; \
    classConfig(&c, 3, 10); \
    change; \
    expectVerdict(&c, GATE_ERR, #change); \
} while (0)

#define ACCEPTED(change) do { \
    gateConfig c; \
    classConfig(&c, 3, 10); \
    change; \
    expectVerdict(&c, GATE_OK, #change); \
} while (0)

TEST(config_validate) {
    char long64[65], long65[66], tiny[8];
    gateConfig cfg;

    memset(long64, 'x', 64);
    long64[64] = '\0';
    memset(long65, 'x', 65);
    long65[65] = '\0';

    ACCEPTED((void)0);
    REFUSED(c.kind = 2);
    REFUSED(c.kind = -1);
    REFUSED(c.alpha = 0.0);
    REFUSED(c.alpha = 1.0);
    REFUSED(c.alpha = -0.1);
    REFUSED(c.alpha = NAN);
    ACCEPTED(c.alpha = 0.999);
    REFUSED(c.delta = 0.0);
    REFUSED(c.delta = 1.0);
    REFUSED(c.delta = NAN);
    ACCEPTED(c.delta = 1e-9);
    REFUSED(c.audit = 0.0);
    REFUSED(c.audit = 1.0 + 1e-9);
    REFUSED(c.audit = NAN);
    ACCEPTED(c.audit = 1.0);
    REFUSED(c.snapshot_examples = 0);
    ACCEPTED(c.snapshot_examples = 1);
    ACCEPTED(c.snapshot_examples = GATE_MAX_SNAPSHOT_EXAMPLES);
    REFUSED(c.snapshot_examples = GATE_MAX_SNAPSHOT_EXAMPLES + 1);
    REFUSED(c.snapshot_examples = UINT32_MAX);
    REFUSED(c.feedback_timeout_ms = 0);
    REFUSED(c.feedback_timeout_ms = -5);
    REFUSED(c.feedback_timeout_ms = GATE_MAX_FEEDBACK_TIMEOUT_MS + 1);
    ACCEPTED(c.feedback_timeout_ms = 1);
    ACCEPTED(c.feedback_timeout_ms = GATE_MAX_FEEDBACK_TIMEOUT_MS);

    /* Labels: 1 to 64 bytes of UTF-8 without control characters, distinct. */
    REFUSED(relabel(&c, 1, ""));
    REFUSED(relabel(&c, 1, long65));
    ACCEPTED(relabel(&c, 1, long64));
    REFUSED(relabel(&c, 1, "two\nlines"));
    REFUSED(relabel(&c, 1, "\xff\xfe"));
    REFUSED(relabel(&c, 2, "label0"));
    ACCEPTED(relabel(&c, 2, "Label0"));

    /* A label's own error rate: in (0, alpha]. */
    REFUSED(c.class_alpha[1] = ALPHA + 0.01);
    REFUSED(c.class_alpha[1] = -0.1);
    REFUSED(c.class_alpha[1] = NAN);
    ACCEPTED(c.class_alpha[1] = ALPHA);
    ACCEPTED(c.class_alpha[1] = 0.01);

    /* ALWAYSDEFER: a flag, and something must be left to answer. */
    REFUSED(c.class_defer[0] = 2);
    REFUSED(c.class_defer[0] = c.class_defer[1] = c.class_defer[2] = 1);
    ACCEPTED(c.class_defer[0] = c.class_defer[1] = 1);

    /* 2 to 64 labels for a classifier, none for a score gate. */
    REFUSED(c.kind = GATE_KIND_SCORE);
    classConfig(&cfg, 1, 10);
    expectVerdict(&cfg, GATE_ERR, "one label");
    classConfig(&cfg, 2, 10);
    expectVerdict(&cfg, GATE_OK, "two labels");
    classConfig(&cfg, GATE_MAX_CLASSES, 10);
    expectVerdict(&cfg, GATE_OK, "64 labels");
    classConfig(&cfg, GATE_MAX_CLASSES + 1, 10);
    expectVerdict(&cfg, GATE_ERR, "65 labels");
    baseConfig(&cfg);
    cfg.kind = GATE_KIND_CLASSIFIER;
    expectVerdict(&cfg, GATE_ERR, "a classifier without labels");
    baseConfig(&cfg);
    expectVerdict(&cfg, GATE_OK, "a score gate");

    /* A label rate of 0 means "not set" and becomes the gate's rate. */
    classConfig(&cfg, 3, 10);
    cfg.class_alpha[0] = 0.0;
    cfg.class_alpha[1] = 0.05;
    cfg.class_alpha[2] = 0.0;
    CHECK(gateConfigValidate(&cfg, tiny, sizeof(tiny)) == GATE_OK);
    CHECK(cfg.class_alpha[0] == ALPHA && cfg.class_alpha[1] == 0.05 &&
          cfg.class_alpha[2] == ALPHA);

    /* The message never overruns the buffer it was given. */
    cfg.alpha = 2.0;
    memset(tiny, 'x', sizeof(tiny));
    CHECK(gateConfigValidate(&cfg, tiny, sizeof(tiny)) == GATE_ERR);
    CHECK(tiny[0] != '\0' && memchr(tiny, '\0', sizeof(tiny)) != NULL);
    gateConfigFree(&cfg);
}

TEST(config_copy_and_free) {
    gateConfig src, dst;
    char err[128];
    uint32_t i;

    classConfig(&src, 3, 77);
    src.audit = 0.5;
    src.feedback_timeout_ms = 4242;
    src.class_alpha[1] = 0.1;
    src.class_defer[2] = 1;
    CHECK(gateConfigValidate(&src, err, sizeof(err)) == GATE_OK);

    memset(&dst, 0xff, sizeof(dst));
    CHECK(gateConfigCopy(&dst, &src) == GATE_OK);
    CHECK_EQ_INT(dst.kind, GATE_KIND_CLASSIFIER);
    CHECK(dst.alpha == ALPHA && dst.delta == DELTA && dst.audit == 0.5);
    CHECK_EQ_INT(dst.snapshot_examples, 77);
    CHECK_EQ_INT(dst.feedback_timeout_ms, 4242);
    CHECK_EQ_INT(dst.nclasses, 3);
    CHECK(dst.labels != src.labels && dst.class_alpha != src.class_alpha &&
          dst.class_defer != src.class_defer);
    for (i = 0; i < 3; i++) {
        CHECK(dst.labels[i] != src.labels[i]);
        CHECK_EQ_STR(dst.labels[i], src.labels[i]);
        CHECK(dst.class_alpha[i] == src.class_alpha[i]);
        CHECK(dst.class_defer[i] == src.class_defer[i]);
    }

    /* Deep: the copy survives changes to, and the release of, the source. */
    src.labels[0][0] = 'L';
    src.class_alpha[1] = 0.15;
    gateConfigFree(&src);
    CHECK_EQ_STR(dst.labels[0], "label0");
    CHECK(dst.class_alpha[1] == 0.1 && dst.class_defer[2] == 1);
    CHECK(gateConfigValidate(&dst, err, sizeof(err)) == GATE_OK);
    gateConfigFree(&dst);
    CHECK(dst.labels == NULL && dst.nclasses == 0);

    baseConfig(&src);
    CHECK(gateConfigCopy(&dst, &src) == GATE_OK);
    CHECK(dst.kind == GATE_KIND_SCORE && dst.alpha == ALPHA && dst.labels == NULL &&
          dst.class_alpha == NULL && dst.class_defer == NULL && dst.nclasses == 0);
    gateConfigFree(&dst);
    gateConfigFree(&src);
}

/* ------------------------------ Thresholds ------------------------------- */

/* SPEC.md section 5: 32 thresholds evenly spaced in log(1 - threshold),
 * from 0 to 0.998 (score) or from max(1/classes, 0.25) to 0.999. */
TEST(grids) {
    static const uint32_t classes[] = {0, 2, 3, 4, 5, 64};
    size_t i;
    uint32_t t;

    for (i = 0; i < COUNTOF(classes); i++) {
        uint32_t k = classes[i];
        gate *g = k ? classGate(k, 10) : scoreGate();
        double lo = k == 0 ? 0.0 : (k == 2 ? 0.5 : (k == 3 ? 1.0 / 3.0 : 0.25));
        double hi = k == 0 ? 0.998 : 0.999;

        CHECK_EQ_INT(g->nrows, k ? k : 1);
        CHECK(g->grid[0] == lo);
        CHECK(g->grid[T - 1] == hi);
        for (t = 0; t < T; t++) {
            double expect = 1.0 - (1.0 - lo) * pow((1.0 - hi) / (1.0 - lo), t / 31.0);

            CHECK_NEAR(g->grid[t], expect, 1e-12);
            CHECK(t == 0 || g->grid[t] > g->grid[t - 1]);
        }
        gateFree(g);
    }
}

/* --------------------------------- The bar -------------------------------
 * bar = log(g (g + 1)) + log(tested rows * 32) - log(delta): generation g
 * may spend delta / (g (g + 1)), split evenly over every threshold of every
 * tested row, because every threshold is a test of its own. */

TEST(bar) {
    static const uint64_t gens[] = {1, 2, 3, 10, 1000, 1ull << 20, 1ull << 40};
    static const struct {
        uint32_t classes, deferred; /* 0 classes: a score gate */
        double delta;
        double wealth[3];           /* to reach, generation 1, 2, 3 */
    } cases[] = {
        {0, 0, 0.2, {320.0, 960.0, 1920.0}},
        {0, 0, 0.01, {6400.0, 19200.0, 38400.0}},
        {2, 0, 0.2, {640.0, 1920.0, 3840.0}},
        {3, 0, 0.2, {960.0, 2880.0, 5760.0}},
        {3, 1, 0.2, {640.0, 1920.0, 3840.0}},      /* ALWAYSDEFER takes no share */
        {3, 2, 0.2, {320.0, 960.0, 1920.0}},
        {64, 0, 0.2, {20480.0, 61440.0, 122880.0}},
        {64, 63, 0.2, {320.0, 960.0, 1920.0}},
        {5, 3, 0.05, {2560.0, 7680.0, 15360.0}}
    };
    size_t c, i;
    uint32_t r;

    for (c = 0; c < COUNTOF(cases); c++) {
        uint32_t tested = cases[c].classes ? cases[c].classes - cases[c].deferred : 1;
        gateConfig cfg;
        gate *g;

        if (cases[c].classes) classConfig(&cfg, cases[c].classes, 3); else baseConfig(&cfg);
        cfg.delta = cases[c].delta;
        for (r = 0; r < cases[c].deferred; r++) cfg.class_defer[cases[c].classes - 1 - r] = 1;
        g = create(&cfg);
        CHECK_EQ_INT(gateTestedRows(g), tested);
        for (i = 0; i < 3; i++)
            CHECK_NEAR(gateBar(g, i + 1), log(cases[c].wealth[i]), 1e-12);
        for (i = 0; i < COUNTOF(gens); i++) {
            double dg = (double)gens[i];

            CHECK_NEAR(gateBar(g, gens[i]),
                       log(dg) + log(dg + 1.0) + log(32.0 * tested) - log(cases[c].delta), 1e-9);
            CHECK_NEAR(gateBar(g, gens[i]), barOf(gens[i], tested, cases[c].delta), 1e-9);
            /* Never easier for a later generation. */
            CHECK(gateBar(g, gens[i] + 1) > gateBar(g, gens[i]));
        }
        /* There is no generation 0: no wealth reaches its bar. */
        CHECK(!(gateBar(g, 0) < INFINITY));
        /* It is the bar the generations of this gate are held to. */
        if (cases[c].classes)
            for (i = 0; i < 3; i++) train(g, T0, &XA, 0);
        CHECK(g->candidate != NULL && candidateOf(g)->gen == 1);
        CHECK(candidateOf(g)->log_threshold == gateBar(g, 1));
        gateRecover(g, T0, EPOCH2);
        CHECK(g->candidate != NULL && candidateOf(g)->gen == 2);
        CHECK(candidateOf(g)->log_threshold == gateBar(g, 2));
        CHECK_NEAR(candidateOf(g)->log_threshold, log(cases[c].wealth[1]), 1e-12);
        inv(g);
        gateFree(g);
    }
}

/* ------------------------------ Score gate ------------------------------- */

TEST(score_life) {
    const double bar = barOf(1, 1, DELTA);
    const uint32_t need = needCorrect(ALPHA, bar, 0);
    gate *g = scoreGate();
    gatePredictResult r, spot, low, top;
    const gateObs *o;
    int64_t now = T0;
    uint32_t i, t;

    /* Nothing to freeze: generation 1 is under test from the first moment. */
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    CHECK(g->candidate != NULL && g->serving == NULL);
    CHECK_EQ_INT(candidateOf(g)->gen, 1);
    CHECK_EQ_INT(g->generations, 1);
    CHECK_EQ_INT(candidateOf(g)->frozen_ms, T0);
    CHECK_EQ_INT(candidateOf(g)->promoted_ms, 0);
    CHECK_EQ_INT(candidateOf(g)->budget, 0);
    CHECK_NEAR(candidateOf(g)->log_threshold, log(320.0), 1e-12);
    CHECK_EQ_INT(candidateOf(g)->policy[0], -1);
    CHECK(g->lineage == LINEAGE && g->epoch == EPOCH);
    CHECK(g->created_ms == T0 && g->now_ms == T0);
    CHECK(g->trainer.w == NULL);
    CHECK(gateRowThreshold(g, 0) < 0.0);
    CHECK(gateCoverage(g) == 0.0);
    CHECK(gateRowAlpha(g, 0) == ALPHA);
    CHECK_EQ_INT(gateRowTested(g, 0), 1);
    CHECK_EQ_INT(gateTestedRows(g), 1);
    CHECK(gateBar(g, 1) == candidateOf(g)->log_threshold);
    CHECK(g->blind == 0 && g->missing_run == 0 && g->nanswered == 0);
    CHECK_EQ_INT(gateUnanswered(g), 0);

    /* While nothing answers, every request is proof, whatever the draw.
     * 0.5 belongs to the regions 0..3 (thresholds 0, 0.18, 0.33, 0.45). */
    CHECK_EQ_INT(regionsOf(g, 0.5), 4);
    r = ask(g, now, 0.0, 0.5);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq == 1 && r.label == -1);
    CHECK_EQ_INT(flagsOf(g, 1), GATE_OBS_AUDIT | GATE_OBS_CAND);
    r = ask(g, now, 0.2499, 0.5);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq == 2);
    CHECK_EQ_INT(flagsOf(g, 2), GATE_OBS_AUDIT | GATE_OBS_CAND);
    r = ask(g, now, AUDIT, 0.5); /* a draw equal to the rate is not a spot check */
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq == 3);
    CHECK_EQ_INT(flagsOf(g, 3), GATE_OBS_CAND);
    o = gateFindObservation(g, 3);
    CHECK(o != NULL && o->cand_gen == 1 && o->serv_gen == 0 && o->cand_label == -1 &&
          o->serv_label == -1 && o->cand_score == 0.5 && o->result == -1 &&
          o->created_ms == now && o->x.n == 0);
    CHECK_EQ_INT(g->npending, 3);
    CHECK_EQ_INT(g->nevidence, 3);

    /* Three wrong answers: the regions 0..3 are in debt from now on. */
    for (i = 1; i <= 3; i++) CHECK_EQ_INT(answer(g, now, i, 0), 1);
    CHECK_EQ_INT(g->npending, 0);
    CHECK_EQ_INT(candidateOf(g)->samples, 3);
    CHECK_EQ_INT(candidateOf(g)->errors, 3);
    for (t = 0; t < T; t++) {
        CHECK_EQ_INT(candidateOf(g)->regions[t].n, t < 4 ? 3 : 0);
        CHECK_EQ_INT(candidateOf(g)->regions[t].errors, t < 4 ? 3 : 0);
    }

    /* Correct answers at 0.9 feed the regions 0..11. The regions 4..11
     * have seen nothing else: they are proven by answer number 35, and not
     * by number 34. */
    CHECK_EQ_INT(need, 35);
    CHECK_EQ_INT(regionsOf(g, 0.9), 12);
    for (i = 1; i <= need; i++) {
        r = ask(g, now, (i & 1) ? 0.999 : 0.0, 0.9);
        CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0);
        CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
        CHECK((gateState(g) == GATE_STATE_ACTIVE) == (i == need));
    }
    CHECK(g->serving != NULL && g->candidate == NULL);
    CHECK_EQ_INT(servingOf(g)->gen, 1);
    CHECK_EQ_INT(g->generations, 1);
    CHECK_EQ_INT(servingOf(g)->promoted_ms, now);
    CHECK_EQ_INT(g->stats.promotions, 1);
    CHECK_EQ_INT(servingOf(g)->samples, 38);
    CHECK_EQ_INT(servingOf(g)->errors, 3);
    for (t = 0; t < T; t++) {
        const gateRegion *reg = &servingOf(g)->regions[t];

        CHECK_EQ_INT(reg->certified_at, t >= 4 && t < 12 ? 35 : 0);
        CHECK_EQ_INT(reg->n, t < 4 ? 38 : (t < 12 ? 35 : 0));
    }
    CHECK_EQ_INT(servingOf(g)->policy[0], 4);
    CHECK(gateRowThreshold(g, 0) == g->grid[4]);
    CHECK_NEAR(g->grid[4], 0.5515171854920451, 1e-12);

    /* ACTIVE. Above the threshold: answered, unless the draw makes it a
     * spot check. Nothing is recorded for an ACCEPT. */
    r = ask(g, now, U_PLAIN, 0.9);
    CHECK(r.action == GATE_ACTION_ACCEPT && r.seq == 0 && r.label == -1);
    CHECK_EQ_INT(g->npending, 0);
    spot = ask(g, now, U_SPOT, 0.9);
    CHECK(spot.action == GATE_ACTION_AUDIT && spot.seq != 0);
    CHECK_EQ_INT(flagsOf(g, spot.seq), GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_SERV);
    o = gateFindObservation(g, spot.seq);
    CHECK(o != NULL && o->serv_gen == 1 && o->cand_gen == 0 && o->serv_score == 0.9);
    /* Below it: not answered, and wanted as proof only if spot checked. */
    r = ask(g, now, U_PLAIN, 0.5);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq == 0);
    low = ask(g, now, U_SPOT, 0.5);
    CHECK(low.action == GATE_ACTION_ABSTAIN && low.seq != 0);
    CHECK_EQ_INT(flagsOf(g, low.seq), GATE_OBS_AUDIT | GATE_OBS_SERV);
    /* The threshold itself is inside, the next double below is not. */
    r = ask(g, now, AUDIT, g->grid[4]);
    CHECK(r.action == GATE_ACTION_ACCEPT && r.seq == 0);
    r = ask(g, now, U_PLAIN, nextafter(g->grid[4], 0.0));
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq == 0);
    top = ask(g, now, nextafter(AUDIT, 0.0), 1.0);
    CHECK(top.action == GATE_ACTION_AUDIT && top.seq != 0);

    CHECK_EQ_INT(g->stats.requests, 45);
    CHECK_EQ_INT(g->stats.accepted, 2);
    CHECK_EQ_INT(g->stats.audited, 2);
    CHECK_EQ_INT(g->stats.abstained, 41);
    CHECK_EQ_INT(g->stats.observations, 41);
    CHECK_EQ_INT(g->stats.learned, 38);
    CHECK_EQ_INT(g->npending, 3);
    CHECK_NEAR(gateCoverage(g), 4.0 / 7.0, 1e-12); /* 4 of the last 7 inside */

    /* A spot check of an answer counts as an audit; proof from below the
     * threshold does not, and never reaches the regions above its score. */
    CHECK_EQ_INT(answer(g, now, spot.seq, 1), 1);
    CHECK_EQ_INT(servingOf(g)->samples, 39);
    CHECK_EQ_INT(servingOf(g)->audits, 1);
    CHECK_EQ_INT(answer(g, now, low.seq, 0), 1);
    CHECK_EQ_INT(servingOf(g)->samples, 40);
    CHECK_EQ_INT(servingOf(g)->errors, 4);
    CHECK_EQ_INT(servingOf(g)->audits, 1);
    CHECK_EQ_INT(servingOf(g)->regions[0].errors, 4);
    CHECK_EQ_INT(servingOf(g)->regions[4].errors, 0);
    CHECK_EQ_INT(answer(g, now, top.seq, 1), 1);
    CHECK_EQ_INT(servingOf(g)->audits, 2);
    CHECK_EQ_INT(servingOf(g)->audit_errors, 0);
    CHECK_EQ_INT(servingOf(g)->regions[T - 1].n, 1); /* 1.0 is in every region */
    /* The drift detector numbered the two spot checks of answers when they
     * were asked; neither disagreed. */
    expectDrift(&servingOf(g)->drift[0], 2, 0, 0, 0);
    CHECK_EQ_INT(g->stats.learned, 41);
    CHECK_EQ_INT(g->npending, 0);
    CHECK_EQ_INT(servingOf(g)->policy[0], 4);
    gateFree(g);
}

/* After promotion the lower thresholds keep being tested on the spot
 * checks: the policy moves down as they are proven, and never back up. */
TEST(continued_testing) {
    const double bar = barOf(1, 1, DELTA);
    const uint32_t n0 = needCorrect(ALPHA, bar, 0), n1 = needCorrect(ALPHA, bar, 1),
                   n2 = needCorrect(ALPHA, bar, 2);
    gate *g = scoreGate();
    gatePredictResult r;
    int64_t now = T0;
    uint32_t i;

    CHECK(n0 == 35 && n1 == 47 && n2 == 57);
    feed(g, now, U_PLAIN, 0.5, 0); /* one wrong answer in the regions 0..3 */
    feed(g, now, U_PLAIN, 0.1, 0); /* and a second one in region 0 */
    for (i = 1; i <= n0; i++) feed(g, now, U_PLAIN, 0.9, 1);
    CHECK(gateState(g) == GATE_STATE_ACTIVE);
    CHECK_EQ_INT(servingOf(g)->policy[0], 4);

    /* 0.3 is under the threshold 0.55: not answered, not proof. */
    for (i = 0; i < 4; i++) {
        r = ask(g, now, U_PLAIN, 0.3);
        CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq == 0);
    }
    CHECK(gateCoverage(g) == 0.0);

    /* Spot checks feed every region their score belongs to. The regions
     * 1..3 carry one wrong answer and are proven by correct answer 47. */
    for (i = n0 + 1; i <= n1; i++) {
        r = ask(g, now, U_SPOT, 0.9);
        CHECK(r.action == GATE_ACTION_AUDIT);
        CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
        CHECK_EQ_INT(servingOf(g)->policy[0], i < n1 ? 4 : 1);
    }
    CHECK(gateRowThreshold(g, 0) == g->grid[1]);
    CHECK_NEAR(gateCoverage(g), 12.0 / 16.0, 1e-12);
    for (i = 0; i < 4; i++) {
        r = ask(g, now, U_PLAIN, 0.3); /* now above the threshold 0.18 */
        CHECK(r.action == GATE_ACTION_ACCEPT);
    }
    CHECK_NEAR(gateCoverage(g), 16.0 / 20.0, 1e-12);

    /* Region 0 carries two wrong answers: correct answer 57. */
    for (i = n1 + 1; i <= n2; i++) {
        r = ask(g, now, U_SPOT, 0.9);
        CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
        CHECK_EQ_INT(servingOf(g)->policy[0], i < n2 ? 1 : 0);
    }
    CHECK(gateRowThreshold(g, 0) == 0.0);
    r = ask(g, now, U_PLAIN, 0.0);
    CHECK(r.action == GATE_ACTION_ACCEPT);
    CHECK_NEAR(gateCoverage(g), 27.0 / 31.0, 1e-12);
    CHECK_EQ_INT(servingOf(g)->regions[4].certified_at, n0);
    CHECK_EQ_INT(servingOf(g)->regions[1].certified_at, n1 + 1);
    CHECK_EQ_INT(servingOf(g)->regions[0].certified_at, n2 + 2);

    /* A proof is "the wealth reached the bar once". Three wrong spot checks
     * take the wealth far below it again; what was proven stays proven. */
    for (i = 0; i < 3; i++) {
        r = ask(g, now, U_SPOT, 0.9);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
    }
    CHECK(!gateRegionCertifies(&servingOf(g)->regions[0], servingOf(g)->log_threshold));
    CHECK_EQ_INT(servingOf(g)->regions[0].certified_at, n2 + 2);
    CHECK_EQ_INT(servingOf(g)->policy[0], 0);
    CHECK_EQ_INT(servingOf(g)->audit_errors, 3);
    r = ask(g, now, U_PLAIN, 0.0);
    CHECK(r.action == GATE_ACTION_ACCEPT);
    CHECK_EQ_INT(g->stats.promotions, 1);
    gateFree(g);
}

/* "coverage": the plain share of requests inside the policy until 1024
 * were seen, then an average that forgets at the rate 1/1024. */
TEST(coverage_average) {
    gate *g = scoreGate();
    int64_t now = T0;
    uint32_t i;
    double c;

    CHECK(gateCoverage(g) == 0.0);
    feed(g, now, U_PLAIN, 0.5, 0);
    for (i = 0; i < 35; i++) feed(g, now, U_PLAIN, 0.9, 1);
    CHECK(g->serving != NULL && servingOf(g)->policy[0] == 4);
    CHECK(gateCoverage(g) == 0.0); /* starts over with the new generation */

    for (i = 0; i < GATE_COVERAGE_WINDOW; i++)
        CHECK(ask(g, now, U_PLAIN, 0.9).action == GATE_ACTION_ACCEPT);
    CHECK(gateCoverage(g) == 1.0);
    for (i = 0; i < 100; i++) CHECK(ask(g, now, U_PLAIN, 0.3).seq == 0);
    c = pow(1.0 - 1.0 / GATE_COVERAGE_WINDOW, 100);
    CHECK_NEAR(gateCoverage(g), c, 1e-12);
    ask(g, now, U_PLAIN, 0.9);
    CHECK_NEAR(gateCoverage(g), c + (1.0 - c) / GATE_COVERAGE_WINDOW, 1e-12);
    gateFree(g);
}

/* ------------------------------ Classifier ------------------------------- */

TEST(evaluate) {
    gate *g = scoreGate();
    float inf_val[] = {INFINITY};
    gateSparse inf = {1, xa_idx, inf_val};
    double score;
    gateEval ev;
    uint32_t i;

    /* Score gate: both generations "guess" the application's score. A
     * generation that is not there guesses nothing: (-1, 0), whatever the
     * caller's variable held (these values are replicated as they are). */
    memset(&ev, 0x5a, sizeof(ev));
    CHECK(gateEvaluate(g, NULL, 0.625, &ev) == GATE_OK);
    CHECK(ev.has_candidate == 1 && ev.has_serving == 0);
    CHECK(ev.candidate.label == -1 && ev.candidate.score == 0.625);
    CHECK(ev.serving.label == -1 && ev.serving.score == 0.0);
    for (i = 0; i < 35; i++) feed(g, T0, U_PLAIN, 0.9, 1);
    memset(&ev, 0x5a, sizeof(ev));
    CHECK(gateEvaluate(g, NULL, 0.25, &ev) == GATE_OK);
    CHECK(ev.has_candidate == 0 && ev.has_serving == 1);
    CHECK(ev.serving.label == -1 && ev.serving.score == 0.25);
    CHECK(ev.candidate.label == -1 && ev.candidate.score == 0.0);
    gateFree(g);

    /* Classifier: nothing to guess with until a snapshot exists. */
    g = classGate(2, 12);
    memset(&ev, 0x5a, sizeof(ev));
    CHECK(gateEvaluate(g, &XA, 0.0, &ev) == GATE_OK);
    CHECK(ev.has_candidate == 0 && ev.has_serving == 0);
    CHECK(ev.candidate.label == -1 && ev.candidate.score == 0.0);
    CHECK(ev.serving.label == -1 && ev.serving.score == 0.0);
    CHECK(gateEvaluate(g, NULL, 0.0, &ev) == GATE_OK);
    for (i = 0; i < 12; i++) train(g, T0, (i & 1) ? &XB : &XA, (int32_t)(i & 1));
    CHECK(gateEvaluate(g, &XB, 0.0, &ev) == GATE_OK);
    CHECK(ev.has_candidate == 1 && ev.has_serving == 0);
    CHECK_EQ_INT(ev.candidate.label, 1);
    CHECK_EQ_INT(gateModelPredict(&candidateOf(g)->model, &XB, NULL, &score), 1);
    CHECK(ev.candidate.score == score);
    CHECK(score > 0.9 && score < 0.92); /* 0.909 after six examples each */
    /* It is the frozen copy that guesses, not the model that keeps learning. */
    for (i = 0; i < 30; i++) train(g, T0, &XB, 0);
    CHECK_EQ_INT(gateModelPredict(&g->trainer, &XB, NULL, NULL), 0);
    CHECK(gateEvaluate(g, &XB, 0.0, &ev) == GATE_OK);
    CHECK(ev.candidate.label == 1 && ev.candidate.score == score);
    /* No clues, or clues the model cannot digest: an error, not a guess. */
    CHECK(gateEvaluate(g, NULL, 0.0, &ev) == GATE_ERR);
    CHECK(gateEvaluate(g, &inf, 0.0, &ev) == GATE_ERR);
    gateFree(g);
}

/* gateEvaluateFor() gives the guesses gateEvaluate() gives, without the
 * candidate's if 'leaves_out': the gate has a candidate then, and the
 * evaluation looks as if it had none. */
static void expectEvaluateFor(const gate *g, const gateSparse *x, double score, double u,
                              int leaves_out)
{
    gateEval full, ev;

    memset(&full, 0x5a, sizeof(full)); /* nothing of what was there may stay */
    memset(&ev, 0x5a, sizeof(ev));
    CHECK(gateEvaluate(g, x, score, &full) == GATE_OK);
    CHECK(gateEvaluateFor(g, x, score, u, &ev) == GATE_OK);
    CHECK_EQ_INT(ev.has_serving, full.has_serving);
    CHECK(ev.serving.label == full.serving.label && ev.serving.score == full.serving.score);
    if (leaves_out) {
        CHECK_EQ_INT(full.has_candidate, 1);
        CHECK_EQ_INT(ev.has_candidate, 0);
        CHECK(ev.candidate.label == -1 && ev.candidate.score == 0.0);
    } else {
        CHECK_EQ_INT(ev.has_candidate, full.has_candidate);
        CHECK(ev.candidate.label == full.candidate.label &&
              ev.candidate.score == full.candidate.score);
    }
}

/* A two-label gate whose generation 1 answers label0 from the third
 * threshold on (0.6652): its first guess, made up at 0.6, was wrong, which
 * leaves the two thresholds below with an error (they need 50 correct
 * answers, the others have their 39). Generation 2 is under test. The
 * frozen model of generation 1 has seen XA five times as label0: it is
 * confident about XA and knows nothing about XB. */
static gate *thirdThresholdGate(int64_t now) {
    gate *g = classGate(2, 5);
    gatePredictResult r;
    gateEval ev;
    uint32_t i;

    for (i = 0; i < 5; i++) train(g, now, &XA, 0);
    for (i = 0; i < 40; i++) {
        ev = guesses(g, -1, 0.0, 0, i ? 0.99 : 0.6);
        r = askGuess(g, now, U_PLAIN, &ev, &XA);
        CHECK_EQ_INT(answer(g, now, r.seq, i ? 0 : 1), 1);
    }
    CHECK(g->serving != NULL && servingOf(g)->gen == 1);
    CHECK(servingOf(g)->policy[0] == 2 && servingOf(g)->policy[1] == -1);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 2);
    return g;
}

/* The evaluation the module makes: the candidate's guess is left out when
 * the serving generation is going to answer the request by itself (not
 * paused, not a spot check, a guess at or above the proven threshold of a
 * label that is answered), and only then. */
TEST(evaluate_for) {
    float inf_val[] = {INFINITY};
    gateSparse inf = {1, xa_idx, inf_val}, x;
    gateConfig cfg;
    gate *g, *twin;
    gatePredictResult r;
    gateEval ev, full;
    const gateObs *o;
    int64_t now = T0;
    uint64_t before;
    double sa, sb, sc;
    uint32_t i;

    /* A score gate has one generation at a time: nothing to leave out. */
    g = scoreGate();
    expectEvaluateFor(g, NULL, 0.9, U_PLAIN, 0);
    expectEvaluateFor(g, NULL, 0.9, U_SPOT, 0);
    for (i = 0; i < 35; i++) feed(g, now, U_PLAIN, 0.9, 1);
    CHECK(g->serving != NULL && g->candidate == NULL);
    expectEvaluateFor(g, NULL, 0.9, U_PLAIN, 0);
    expectEvaluateFor(g, NULL, 0.9, U_SPOT, 0);
    expectEvaluateFor(g, NULL, 0.3, U_PLAIN, 0);
    gateFree(g);

    /* A classifier without a generation, and with a candidate only. */
    g = classGate(2, 12);
    expectEvaluateFor(g, &XA, 0.0, U_PLAIN, 0);
    CHECK(gateEvaluateFor(g, NULL, 0.0, U_PLAIN, &ev) == GATE_OK);
    for (i = 0; i < 12; i++) train(g, now, (i & 1) ? &XB : &XA, (int32_t)(i & 1));
    CHECK(g->serving == NULL && g->candidate != NULL);
    expectEvaluateFor(g, &XA, 0.0, U_PLAIN, 0);
    expectEvaluateFor(g, &XA, 0.0, U_SPOT, 0);
    /* As gateEvaluate(): no clues, or clues the model cannot digest. */
    CHECK(gateEvaluateFor(g, NULL, 0.0, U_PLAIN, &ev) == GATE_ERR);
    CHECK(gateEvaluateFor(g, &inf, 0.0, U_PLAIN, &ev) == GATE_ERR);
    gateFree(g);

    /* Both generations. The serving one answers XA (0.898 for label0, the
     * threshold is 0.6652)... */
    g = thirdThresholdGate(now);
    before = fingerprint(g, 0);
    CHECK_EQ_INT(gateModelPredict(&servingOf(g)->model, &XA, NULL, &sa), 0);
    CHECK_EQ_INT(gateModelPredict(&servingOf(g)->model, &XB, NULL, &sb), 0);
    CHECK(sa >= g->grid[2] && sb >= g->grid[0] && sb < g->grid[2]);
    expectEvaluateFor(g, &XA, 0.0, U_PLAIN, 1);
    /* ...unless the request is a spot check: a draw below AUDIT, */
    expectEvaluateFor(g, &XA, 0.0, U_SPOT, 0);
    expectEvaluateFor(g, &XA, 0.0, 0.0, 0);
    expectEvaluateFor(g, &XA, 0.0, nextafter(AUDIT, 0.0), 0);
    expectEvaluateFor(g, &XA, 0.0, AUDIT, 1);
    /* ...and not XB, whose guess (0.532 for label0) is below the threshold. */
    expectEvaluateFor(g, &XB, 0.0, U_PLAIN, 0);
    CHECK(gateEvaluateFor(g, NULL, 0.0, U_PLAIN, &ev) == GATE_ERR);
    CHECK(gateEvaluateFor(g, &inf, 0.0, U_PLAIN, &ev) == GATE_ERR);
    CHECK(gateEvaluateFor(g, &inf, 0.0, U_SPOT, &ev) == GATE_ERR);
    CHECK(fingerprint(g, 0) == before); /* pure */

    /* The request is answered on the reduced evaluation as it is on the
     * full one, and the gate is the same afterwards. */
    twin = thirdThresholdGate(now);
    CHECK(fingerprint(twin, 0) == before);
    CHECK(gateEvaluateFor(g, &XA, 0.0, U_PLAIN, &ev) == GATE_OK);
    CHECK(gateEvaluate(twin, &XA, 0.0, &full) == GATE_OK);
    CHECK(ev.has_candidate == 0 && full.has_candidate == 1);
    CHECK_EQ_INT(gatePredictNeedsFeatures(g, U_PLAIN, &ev), 0);
    CHECK(gatePredict(g, now, U_PLAIN, &ev, NULL, &r) == GATE_OK);
    CHECK(r.action == GATE_ACTION_ACCEPT && r.label == 0 && r.seq == 0);
    CHECK(gatePredict(twin, now, U_PLAIN, &full, NULL, &r) == GATE_OK);
    CHECK(r.action == GATE_ACTION_ACCEPT && r.label == 0 && r.seq == 0);
    CHECK(fingerprint(g, 0) == fingerprint(twin, 0));
    CHECK(fingerprint(g, 0) != before);
    CHECK_EQ_INT(g->stats.accepted, 1);
    CHECK_EQ_INT(g->npending, 0);
    inv(g);
    gateFree(twin);

    /* It is good for that draw only: a spot check of the same clues needs
     * the candidate's guess, and without it nothing happens. */
    before = fingerprint(g, 0);
    if (gateSparseCopy(&x, &XA) != GATE_OK) die("out of memory");
    CHECK_EQ_INT(gatePredictNeedsFeatures(g, U_SPOT, &ev), 0);
    CHECK(gatePredict(g, now, U_SPOT, &ev, &x, &r) == GATE_ERR);
    CHECK(r.seq == 0);
    CHECK(fingerprint(g, 0) == before);
    CHECK(x.n == 1 && x.idx != NULL);
    /* Evaluated for its own draw it is recorded with both guesses. */
    CHECK(gateEvaluateFor(g, &XA, 0.0, U_SPOT, &ev) == GATE_OK);
    CHECK_EQ_INT(gatePredictNeedsFeatures(g, U_SPOT, &ev), 1);
    CHECK(gatePredict(g, now, U_SPOT, &ev, &x, &r) == GATE_OK);
    CHECK(r.action == GATE_ACTION_AUDIT && r.seq != 0);
    CHECK(x.n == 0 && x.idx == NULL);
    o = gateFindObservation(g, r.seq);
    CHECK(o != NULL && o->serv_gen == 1 && o->cand_gen == 2);
    CHECK(o != NULL && o->serv_label == 0 && o->serv_score == sa);
    CHECK_EQ_INT(gateModelPredict(&candidateOf(g)->model, &XA, NULL, &sc), 0);
    CHECK(o != NULL && o->cand_label == 0 && o->cand_score == sc && sc != sa);
    CHECK_EQ_INT(flagsOf(g, r.seq),
                 GATE_OBS_AUDIT | GATE_OBS_SERV | GATE_OBS_CAND | GATE_OBS_WOULD_ACCEPT);
    inv(g);

    /* A paused gate answers nothing: the candidate's guess is wanted for
     * the record. */
    gatePause(g, now);
    expectEvaluateFor(g, &XA, 0.0, U_PLAIN, 0);
    r = askText(g, now, U_PLAIN, &XA);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0);
    o = gateFindObservation(g, r.seq);
    CHECK(o != NULL && o->cand_gen == 2 && o->cand_label == 0 && o->cand_score == sc);
    gateResume(g, now);
    expectEvaluateFor(g, &XA, 0.0, U_PLAIN, 1);
    CHECK(askText(g, now, U_PLAIN, &XA).action == GATE_ACTION_ACCEPT);
    gateFree(g);

    /* A label without a proven threshold is not answered. */
    g = trainedGate(12);
    for (i = 0; i < 39; i++) feedText(g, now, U_PLAIN, &XA, 0);
    CHECK(g->serving != NULL && g->candidate != NULL);
    CHECK(servingOf(g)->policy[0] == 0 && servingOf(g)->policy[1] == -1);
    CHECK_EQ_INT(gateModelPredict(&servingOf(g)->model, &XB, NULL, NULL), 1);
    expectEvaluateFor(g, &XB, 0.0, U_PLAIN, 0);
    expectEvaluateFor(g, &XA, 0.0, U_PLAIN, 1);
    /* A guess exactly on the threshold is answered. A model that has
     * learned nothing says label0 with 0.5, the lowest threshold. */
    gateModelReset(&g->serving->model);
    CHECK_EQ_INT(gateModelPredict(&servingOf(g)->model, &XA, NULL, &sa), 0);
    CHECK(sa == 0.5 && g->grid[0] == 0.5);
    expectEvaluateFor(g, &XA, 0.0, U_PLAIN, 1);
    r = askText(g, now, U_PLAIN, &XA);
    CHECK(r.action == GATE_ACTION_ACCEPT && r.label == 0);
    gateFree(g);

    /* A label that is ALWAYSDEFER is not answered either. */
    classConfig(&cfg, 3, 12);
    cfg.class_defer[2] = 1;
    g = create(&cfg);
    for (i = 0; i < 12; i++) train(g, now, (i & 1) ? &XC : &XA, (i & 1) ? 2 : 0);
    for (i = 0; i < 39; i++) feedText(g, now, U_PLAIN, &XA, 0);
    CHECK(g->serving != NULL && g->candidate != NULL);
    CHECK(servingOf(g)->policy[0] == 0 && servingOf(g)->policy[2] == -1);
    CHECK_EQ_INT(gateModelPredict(&servingOf(g)->model, &XC, NULL, NULL), 2);
    expectEvaluateFor(g, &XC, 0.0, U_PLAIN, 0);
    expectEvaluateFor(g, &XA, 0.0, U_PLAIN, 1);
    gateFree(g);
}

/* runtime_tag and runtime_clock belong to the embedding layer: the core
 * creates them as 0 and never touches them, whatever happens to the gate. */
#define EMBEDDING_TAG 0x7461676765647461ull
#define EMBEDDING_CLOCK (INT64_MIN + 5)
#define EMBEDDING_KEPT(g) \
    CHECK((g)->runtime_tag == EMBEDDING_TAG && (g)->runtime_clock == EMBEDDING_CLOCK)

TEST(embedding_fields_are_left_alone) {
    int kind;

    for (kind = 0; kind < 2; kind++) {
        gate *g = kind ? trainedGate(12) : scoreGate();
        gatePredictResult a, b;
        int64_t t = T0;
        uint32_t i;

        CHECK(g->runtime_tag == 0 && g->runtime_clock == 0);
        g->runtime_tag = EMBEDDING_TAG;
        g->runtime_clock = EMBEDDING_CLOCK;
        /* Proof (35 correct answers for the score gate, 39 for the two
         * labels), a promotion, an ACCEPT. */
        for (i = 0; i < (kind ? 39u : 35u); i++) {
            if (kind) feedText(g, t, U_PLAIN, &XA, 0); else feed(g, t, U_PLAIN, 0.9, 1);
        }
        CHECK(g->serving != NULL && g->stats.promotions == 1);
        a = kind ? askText(g, t, U_PLAIN, &XA) : ask(g, t, U_PLAIN, 0.9);
        CHECK(a.action == GATE_ACTION_ACCEPT);
        EMBEDDING_KEPT(g);
        /* Proof that moves on, a late answer, a timeout. */
        a = kind ? askText(g, t, U_SPOT, &XA) : ask(g, t, U_SPOT, 0.9);
        b = kind ? askText(g, t, U_SPOT, &XA) : ask(g, t, U_SPOT, 0.9);
        CHECK_EQ_INT(answer(g, t, b.seq, kind ? 0 : 1), 1);
        gateMaintain(g, t + GRACE);
        CHECK_EQ_INT(flagsOf(g, a.seq) & GATE_OBS_PROVISIONAL, GATE_OBS_PROVISIONAL);
        CHECK_EQ_INT(answer(g, t + GRACE, a.seq, kind ? 0 : 1), 1);
        a = kind ? askText(g, t + GRACE, U_SPOT, &XA) : ask(g, t + GRACE, U_SPOT, 0.9);
        t += GRACE + TIMEOUT;
        gateMaintain(g, t);
        CHECK_EQ_INT(g->stats.missing, 1);
        EMBEDDING_KEPT(g);
        /* A blind alarm and its end. */
        for (i = 0; i < GATE_MISSING_RUN; i++) {
            a = kind ? askText(g, t, U_SPOT, &XA) : ask(g, t, U_SPOT, 0.9);
            CHECK(a.seq != 0);
        }
        t += TIMEOUT;
        gateMaintain(g, t);
        CHECK(g->blind == 1 && g->serving == NULL && g->stats.revocations == 1);
        EMBEDDING_KEPT(g);
        a = kind ? askText(g, t, U_PLAIN, &XA) : ask(g, t, U_PLAIN, 0.9);
        CHECK_EQ_INT(answer(g, t, a.seq, kind ? 0 : 1), 1);
        CHECK(g->blind == 0 && g->candidate != NULL);
        EMBEDDING_KEPT(g);
        /* The administrator's commands. */
        if (kind) train(g, t, &XB, 1);
        gatePause(g, t);
        EMBEDDING_KEPT(g);
        gateResume(g, t);
        EMBEDDING_KEPT(g);
        gateReset(g, t, EPOCH2);
        inv(g);
        CHECK_EQ_INT(g->epoch, EPOCH2);
        EMBEDDING_KEPT(g);
        gateRecover(g, t - 5, EPOCH3);
        inv(g);
        CHECK_EQ_INT(g->epoch, EPOCH3);
        EMBEDDING_KEPT(g);
        gateFree(g);
    }
}

/* Three labels: "label1" has a stricter error rate, "label2" is ALWAYSDEFER. */
TEST(classifier_life) {
    gateConfig cfg;
    gate *g;
    gatePredictResult r, early[4];
    const gateObs *o;
    int64_t now = T0;
    double sa, sb, sc;
    uint32_t i, t, need_a, need_b;

    classConfig(&cfg, 3, 240);
    cfg.class_alpha[1] = 0.1;
    cfg.class_defer[2] = 1;
    g = create(&cfg);

    CHECK(gateState(g) == GATE_STATE_COLD);
    CHECK(g->candidate == NULL && g->serving == NULL);
    CHECK_EQ_INT(g->generations, 0);
    CHECK(g->trainer.w != NULL && g->trainer.updates == 0);
    CHECK_EQ_INT(g->nrows, 3);
    CHECK(gateRowAlpha(g, 0) == ALPHA && gateRowAlpha(g, 1) == 0.1);
    CHECK(gateRowTested(g, 0) && gateRowTested(g, 1) && !gateRowTested(g, 2));
    CHECK_EQ_INT(gateTestedRows(g), 2);
    CHECK_NEAR(gateBar(g, 1), log(640.0), 1e-12);

    /* COLD: requests are recorded so that their answers can train the
     * model, and that is all they are for, whatever the draw. */
    early[0] = askText(g, now, 0.0, &XA);
    early[1] = askText(g, now, U_PLAIN, &XB);
    early[2] = askText(g, now, U_PLAIN, &XC);
    early[3] = askText(g, now, U_PLAIN, &XA);
    for (i = 0; i < 4; i++) CHECK(early[i].action == GATE_ACTION_ABSTAIN && early[i].seq == i + 1);
    CHECK_EQ_INT(flagsOf(g, 1), GATE_OBS_AUDIT);
    CHECK_EQ_INT(flagsOf(g, 2), 0);
    o = gateFindObservation(g, 1);
    CHECK(o != NULL && o->cand_gen == 0 && o->serv_gen == 0 && o->cand_label == -1 &&
          o->x.n == 1 && o->x.idx[0] == 3 && o->x.val[0] == 4.0f);
    CHECK_EQ_INT(g->nevidence, 0);

    /* The first snapshot is taken at example 240, be it GADD or GLEARN. */
    for (i = 0; i < 237; i++) {
        static const gateSparse *const xs[3] = {&XA, &XB, &XC};

        train(g, now, xs[i % 3], (int32_t)(i % 3));
    }
    CHECK_EQ_INT(answer(g, now, early[0].seq, 0), 1);
    CHECK_EQ_INT(answer(g, now, early[1].seq, 1), 1);
    CHECK_EQ_INT(g->trainer.updates, 239);
    CHECK(gateState(g) == GATE_STATE_COLD);
    CHECK_EQ_INT(answer(g, now, early[2].seq, 2), 1);
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    CHECK(g->candidate != NULL && g->serving == NULL);
    CHECK_EQ_INT(candidateOf(g)->gen, 1);
    CHECK_EQ_INT(g->generations, 1);
    CHECK_EQ_INT(candidateOf(g)->model.updates, 240);
    CHECK_EQ_INT(g->snapshot_updates, 240);
    CHECK(candidateOf(g)->model.w != NULL && candidateOf(g)->model.w != g->trainer.w);
    /* Two tested labels, 32 thresholds each. */
    CHECK_NEAR(candidateOf(g)->log_threshold, log(640.0), 1e-12);
    /* For each of the two tested labels, 10 times the proof a flawless
     * threshold of the most demanding one needs: the stricter label takes
     * 82 correct answers to 640 (file comment). */
    CHECK_EQ_INT(candidateOf(g)->budget, 2 * 10 * 82);
    CHECK_EQ_INT(candidateOf(g)->missed, 0);

    /* A request asked before the snapshot existed is not proof for it. */
    CHECK_EQ_INT(flagsOf(g, early[3].seq), 0);
    CHECK_EQ_INT(answer(g, now, early[3].seq, 0), 1);
    CHECK_EQ_INT(g->trainer.updates, 241);
    CHECK_EQ_INT(candidateOf(g)->samples, 0);

    /* What the frozen model guesses for the three clue vectors. */
    CHECK_EQ_INT(gateModelPredict(&candidateOf(g)->model, &XA, NULL, &sa), 0);
    CHECK_EQ_INT(gateModelPredict(&candidateOf(g)->model, &XB, NULL, &sb), 1);
    CHECK_EQ_INT(gateModelPredict(&candidateOf(g)->model, &XC, NULL, &sc), 2);
    CHECK(regionsOf(g, sa) >= 1 && regionsOf(g, sb) >= 1 && regionsOf(g, sc) >= 1);

    /* CALIBRATING: every request is proof for the label the snapshot
     * guessed, if that label is ever answered. label0 needs 39 correct
     * answers, label1 needs 82; a guess of label2 proves nothing and is
     * counted nowhere: such a request is kept for training only. */
    need_a = needCorrect(ALPHA, log(640.0), 0);
    need_b = needCorrect(0.1, log(640.0), 0);
    CHECK(need_a == 39 && need_b == 82);
    for (i = 1; i < need_a; i++) {
        r = askText(g, now, (i & 1) ? U_SPOT : U_PLAIN, &XA);
        CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0);
        CHECK(flagsOf(g, r.seq) & GATE_OBS_CAND);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
        if (i <= 20) feedText(g, now, U_PLAIN, &XB, 1);
        if (i <= 5) {
            r = askText(g, now, (i & 1) ? U_SPOT : U_PLAIN, &XC);
            CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0);
            CHECK_EQ_INT(flagsOf(g, r.seq), (i & 1) ? GATE_OBS_AUDIT : 0);
            CHECK_EQ_INT(g->nevidence, 0);
            CHECK_EQ_INT(answer(g, now, r.seq, i == 5 ? 0 : 2), 1); /* right or wrong */
        }
    }
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    CHECK_EQ_INT(candidateOf(g)->samples, 38 + 20);
    CHECK_EQ_INT(candidateOf(g)->errors, 0);
    /* Of the five requests for which it guessed label2, the teacher said
     * label0 once: that one it could have been tested on. */
    CHECK_EQ_INT(candidateOf(g)->missed, 1);
    for (t = 0; t < T; t++) {
        CHECK_EQ_INT(candidateOf(g)->regions[0 * T + t].n, t < regionsOf(g, sa) ? 38 : 0);
        CHECK_EQ_INT(candidateOf(g)->regions[1 * T + t].n, t < regionsOf(g, sb) ? 20 : 0);
        CHECK_EQ_INT(candidateOf(g)->regions[2 * T + t].n, 0); /* never tested */
    }
    feedText(g, now, U_PLAIN, &XA, 0); /* the 39th */
    CHECK(gateState(g) == GATE_STATE_ACTIVE);
    CHECK(g->serving != NULL && servingOf(g)->gen == 1 && g->candidate == NULL);
    CHECK_EQ_INT(g->stats.promotions, 1);
    /* Every threshold the score reaches was proven by that answer, each on
     * its own; the gate answers from the lowest of them. */
    for (t = 0; t < T; t++)
        CHECK_EQ_INT(servingOf(g)->regions[0 * T + t].certified_at, t < regionsOf(g, sa) ? 39 : 0);
    CHECK_EQ_INT(servingOf(g)->policy[0], 0);
    CHECK_EQ_INT(servingOf(g)->policy[1], -1);
    CHECK_EQ_INT(servingOf(g)->policy[2], -1);
    CHECK(gateRowThreshold(g, 0) == 1.0 / 3.0);
    CHECK(gateRowThreshold(g, 1) < 0.0 && gateRowThreshold(g, 2) < 0.0);
    CHECK(gateRowThreshold(g, 3) < 0.0); /* no such label */

    /* ACTIVE for label0 only. */
    r = askText(g, now, U_PLAIN, &XA);
    CHECK(r.action == GATE_ACTION_ACCEPT && r.label == 0 && r.seq == 0);
    r = askText(g, now, U_SPOT, &XA);
    CHECK(r.action == GATE_ACTION_AUDIT && r.seq != 0 && r.label == -1);
    CHECK_EQ_INT(flagsOf(g, r.seq), GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_SERV);
    CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
    CHECK_EQ_INT(servingOf(g)->audits, 1);
    /* label1 has no proven threshold: everything goes to the teacher and
     * everything is proof, spot check or not. */
    r = askText(g, now, U_PLAIN, &XB);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0);
    CHECK_EQ_INT(flagsOf(g, r.seq), GATE_OBS_SERV);
    CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
    r = askText(g, now, U_SPOT, &XB);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0);
    CHECK_EQ_INT(flagsOf(g, r.seq), GATE_OBS_AUDIT | GATE_OBS_SERV);
    CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
    CHECK_EQ_INT(servingOf(g)->regions[1 * T].n, 22);
    /* label2 is never answered and never tested: its requests only train. */
    r = askText(g, now, U_PLAIN, &XC);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0);
    CHECK_EQ_INT(flagsOf(g, r.seq), 0);
    CHECK_EQ_INT(answer(g, now, r.seq, 2), 1);
    r = askText(g, now, U_SPOT, &XC);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0);
    CHECK_EQ_INT(flagsOf(g, r.seq), GATE_OBS_AUDIT);
    CHECK_EQ_INT(answer(g, now, r.seq, 2), 1);
    CHECK_EQ_INT(servingOf(g)->samples, 59 + 3);

    /* label1 gets its threshold at its own 82nd correct answer. */
    for (i = 23; i <= need_b; i++) {
        feedText(g, now, (i & 1) ? U_SPOT : U_PLAIN, &XB, 1);
        CHECK_EQ_INT(servingOf(g)->policy[1], i < need_b ? -1 : 0);
    }
    r = askText(g, now, U_PLAIN, &XB);
    CHECK(r.action == GATE_ACTION_ACCEPT && r.label == 1 && r.seq == 0);
    r = askText(g, now, U_PLAIN, &XC);
    CHECK(r.action == GATE_ACTION_ABSTAIN);
    CHECK_EQ_INT(answer(g, now, r.seq, 2), 1);
    for (t = 0; t < T; t++) CHECK_EQ_INT(servingOf(g)->regions[2 * T + t].n, 0);
    CHECK_EQ_INT(servingOf(g)->policy[2], -1);
    CHECK_EQ_INT(g->stats.promotions, 1);
    CHECK(g->candidate == NULL); /* 240 more examples have not been learned */
    gateFree(g);
}

/* ------------------------------ Frozen guesses --------------------------- */

TEST(frozen_guesses) {
    gate *g = trainedGate(12);
    gatePredictResult r1, r2;
    const gateObs *o;
    int64_t now = T0;
    uint64_t before;
    double score;
    uint32_t i;

    CHECK_EQ_INT(gateModelPredict(&candidateOf(g)->model, &XA, NULL, &score), 0);
    r1 = askText(g, now, U_PLAIN, &XA);
    r2 = askText(g, now, U_PLAIN, &XA);
    o = gateFindObservation(g, r1.seq);
    CHECK(o != NULL && o->cand_label == 0 && o->cand_score == score && o->cand_gen == 1);

    /* GADD teaches the live model that XA means label1. No generation's
     * proof, and no frozen model, moves because of it. */
    before = mixGeneration(0, g, g->candidate, 0);
    for (i = 0; i < 60; i++) train(g, now, &XA, 1);
    CHECK_EQ_INT(gateModelPredict(&g->trainer, &XA, NULL, NULL), 1);
    CHECK(mixGeneration(0, g, g->candidate, 0) == before);
    CHECK_EQ_INT(candidateOf(g)->samples, 0);

    /* The teacher says label0: right for the guess recorded with the
     * request, although the live model would now say label1. */
    CHECK_EQ_INT(answer(g, now, r1.seq, 0), 1);
    CHECK_EQ_INT(candidateOf(g)->samples, 1);
    CHECK_EQ_INT(candidateOf(g)->errors, 0);
    CHECK_EQ_INT(candidateOf(g)->regions[0 * T].n, 1);
    CHECK_EQ_INT(candidateOf(g)->regions[1 * T].n, 0);
    /* And label1 is wrong for it, although the live model agrees. */
    CHECK_EQ_INT(answer(g, now, r2.seq, 1), 1);
    CHECK_EQ_INT(candidateOf(g)->errors, 1);
    CHECK_EQ_INT(candidateOf(g)->regions[0 * T].errors, 1);
    CHECK_EQ_INT(candidateOf(g)->regions[1 * T].n, 0);
    gateFree(g);
}

TEST(asked_before_snapshot) {
    gate *g = trainedGate(50);
    gatePredictResult before, after, other;
    int64_t now = T0;
    uint32_t i;

    for (i = 0; i < 39; i++) feedText(g, now, U_PLAIN, &XA, 0);
    CHECK(gateState(g) == GATE_STATE_ACTIVE && g->candidate == NULL);

    /* A spot check asked while generation 1 serves alone... */
    before = askText(g, now, U_SPOT, &XA);
    CHECK(before.action == GATE_ACTION_AUDIT);
    CHECK_EQ_INT(flagsOf(g, before.seq), GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_SERV);
    /* ...then the next snapshot is taken (50 examples after the first: the
     * 39 answers and 11 of these)... */
    for (i = 0; i < 12; i++) {
        CHECK_EQ_INT(g->candidate != NULL, i > 10);
        train(g, now, (i & 1) ? &XB : &XA, (int32_t)(i & 1));
    }
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 2);
    /* ...and the answer is proof for generation 1 only. */
    CHECK_EQ_INT(answer(g, now, before.seq, 0), 1);
    CHECK_EQ_INT(servingOf(g)->samples, 40);
    CHECK_EQ_INT(candidateOf(g)->samples, 0);

    /* With something serving, the candidate's proof is the spot checks. */
    after = askText(g, now, U_SPOT, &XA);
    CHECK_EQ_INT(flagsOf(g, after.seq),
                 GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_SERV | GATE_OBS_CAND);
    other = askText(g, now, U_PLAIN, &XB); /* label1: unproven, not a spot check */
    CHECK_EQ_INT(flagsOf(g, other.seq), GATE_OBS_SERV);
    CHECK_EQ_INT(answer(g, now, after.seq, 0), 1);
    CHECK_EQ_INT(answer(g, now, other.seq, 1), 1);
    CHECK_EQ_INT(candidateOf(g)->samples, 1);
    CHECK_EQ_INT(servingOf(g)->samples, 42);
    gateFree(g);
}

TEST(proof_survives_promotion) {
    gate *g = scoreGate();
    gatePredictResult r[40];
    int64_t now = T0;
    uint32_t i;

    for (i = 0; i < 40; i++) r[i] = ask(g, now, U_PLAIN, 0.9);
    for (i = 0; i < 35; i++) CHECK_EQ_INT(answer(g, now, r[i].seq, 1), 1);
    CHECK(gateState(g) == GATE_STATE_ACTIVE);
    CHECK_EQ_INT(servingOf(g)->samples, 35);
    /* Asked of the candidate, answered when it already serves. */
    for (i = 35; i < 40; i++) CHECK_EQ_INT(answer(g, now, r[i].seq, 1), 1);
    CHECK_EQ_INT(servingOf(g)->samples, 40);
    CHECK_EQ_INT(servingOf(g)->regions[0].n, 40);
    CHECK_EQ_INT(servingOf(g)->audits, 0);
    gateFree(g);
}

/* --------------------------------- Order --------------------------------- */

TEST(answers_wait_for_their_turn) {
    gate *g = scoreGate();
    gatePredictResult r[40], early, proof;
    int64_t now = T0;
    uint32_t i;

    for (i = 0; i < 40; i++) r[i] = ask(g, now, U_PLAIN, 0.9);
    /* Newest first, and before the oldest request is GRACE old: each
     * answer is taken, none is applied. */
    for (i = 39; i >= 1; i--) {
        CHECK_EQ_INT(answer(g, now + GRACE - 1, r[i].seq, 1), 1);
        CHECK_EQ_INT(flagsOf(g, r[i].seq), GATE_OBS_CAND | GATE_OBS_RESOLVED);
        CHECK_EQ_INT(candidateOf(g)->samples, 0);
        CHECK_EQ_INT(g->npending, 40);
        CHECK_EQ_INT(g->nanswered, 40 - i);
        CHECK_EQ_INT(gateUnanswered(g), i);
        CHECK_EQ_INT(g->nevidence, 40);
    }
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    /* The oldest arrives and all 40 are applied, in request order: the
     * 35th proved the threshold. */
    CHECK_EQ_INT(answer(g, now + GRACE - 1, r[0].seq, 1), 1);
    CHECK(gateState(g) == GATE_STATE_ACTIVE);
    CHECK_EQ_INT(servingOf(g)->samples, 40);
    CHECK_EQ_INT(servingOf(g)->regions[0].certified_at, 35);
    CHECK_EQ_INT(g->npending, 0);
    CHECK_EQ_INT(g->nanswered, 0);
    CHECK_EQ_INT(g->stats.learned, 40);
    gateFree(g);

    /* Only proof waits for older proof. A request recorded for training
     * holds nothing back and is held back by nothing. */
    g = classGate(2, 12);
    early = askText(g, now, U_PLAIN, &XA);
    for (i = 0; i < 12; i++) train(g, now, (i & 1) ? &XB : &XA, (int32_t)(i & 1));
    proof = askText(g, now, U_PLAIN, &XA);
    CHECK_EQ_INT(flagsOf(g, early.seq), 0);
    CHECK_EQ_INT(flagsOf(g, proof.seq), GATE_OBS_CAND);
    CHECK_EQ_INT(answer(g, now, proof.seq, 0), 1);
    CHECK_EQ_INT(candidateOf(g)->samples, 1);
    CHECK_EQ_INT(g->npending, 1);
    proof = askText(g, now, U_PLAIN, &XA);
    CHECK_EQ_INT(answer(g, now, early.seq, 0), 1);
    CHECK_EQ_INT(g->npending, 1);
    CHECK(gateFindObservation(g, proof.seq) != NULL);
    gateFree(g);
}

/* Everything about the proof of two gates is the same. */
static void sameProof(const gate *a, const gate *b) {
    const gateGeneration *ga[2], *gb[2];
    uint32_t r;
    int i;

    ga[0] = a->candidate;
    ga[1] = a->serving;
    gb[0] = b->candidate;
    gb[1] = b->serving;
    CHECK(gateState(a) == gateState(b));
    CHECK_EQ_INT(a->generations, b->generations);
    CHECK_EQ_INT(a->npending, b->npending);
    CHECK_EQ_INT(a->nanswered, b->nanswered);
    CHECK_EQ_INT(a->missing_run, b->missing_run);
    CHECK_EQ_INT(a->answer_mark, b->answer_mark);
    CHECK_EQ_INT(a->blind, b->blind);
    CHECK_EQ_INT(a->next_seq, b->next_seq);
    CHECK_EQ_INT(a->stats.promotions, b->stats.promotions);
    CHECK_EQ_INT(a->stats.revocations, b->stats.revocations);
    CHECK_EQ_INT(a->stats.retired, b->stats.retired);
    CHECK_EQ_INT(a->stats.observations, b->stats.observations);
    CHECK(a->stats.coverage == b->stats.coverage);
    for (i = 0; i < 2; i++) {
        CHECK((ga[i] == NULL) == (gb[i] == NULL));
        if (ga[i] == NULL || gb[i] == NULL) continue;
        CHECK_EQ_INT(ga[i]->gen, gb[i]->gen);
        CHECK_EQ_INT(ga[i]->frozen_ms, gb[i]->frozen_ms);
        CHECK_EQ_INT(ga[i]->promoted_ms, gb[i]->promoted_ms);
        CHECK_EQ_INT(ga[i]->samples, gb[i]->samples);
        CHECK_EQ_INT(ga[i]->errors, gb[i]->errors);
        CHECK_EQ_INT(ga[i]->missed, gb[i]->missed);
        CHECK_EQ_INT(ga[i]->audits, gb[i]->audits);
        CHECK_EQ_INT(ga[i]->audit_errors, gb[i]->audit_errors);
        CHECK(memcmp(ga[i]->policy, gb[i]->policy, sizeof(int32_t) * a->nrows) == 0);
        /* Wealth, counts and the sample at which each region crossed. */
        CHECK(memcmp(ga[i]->regions, gb[i]->regions, sizeof(gateRegion) * a->nrows * T) == 0);
        CHECK(memcmp(ga[i]->paired, gb[i]->paired, sizeof(uint64_t) * a->nrows * T) == 0);
        /* The drift detector is no proof: it hears each answer the moment
         * it arrives. But a spot check has the number it was asked under,
         * and a block is a product: the detector holds the same counts
         * whatever the order in which the answers came. */
        for (r = 0; r < a->nrows; r++) {
            int s;

            CHECK_EQ_INT(ga[i]->drift[r].asked, gb[i]->drift[r].asked);
            CHECK_EQ_INT(ga[i]->drift[r].errors, gb[i]->drift[r].errors);
            for (s = 0; s < GATE_DRIFT_SCALES; s++) {
                CHECK_EQ_INT(ga[i]->drift[r].wrong[s], gb[i]->drift[r].wrong[s]);
                CHECK_EQ_INT(ga[i]->drift[r].lost[s], gb[i]->drift[r].lost[s]);
            }
        }
    }
}

/* Answer the recorded requests of one batch: gate 0 in request order, gate
 * 1 newest first, gate 2 in a random order. 'lost' ones get no answer. */
static void answerBatch(gate *g, int which, int64_t now, const uint64_t *seq,
                        const int32_t *result, const int *lost, uint32_t n, uint64_t *rng)
{
    uint32_t order[128], i;

    for (i = 0; i < n; i++) order[i] = which == 1 ? n - 1 - i : i;
    for (i = n - 1; which == 2 && i > 0; i--) {
        uint32_t j = (uint32_t)(gate_test_rand64(rng) % (i + 1)), tmp = order[i];

        order[i] = order[j];
        order[j] = tmp;
    }
    for (i = 0; i < n; i++) {
        uint32_t k = order[i];

        if (seq[k] == 0 || lost[k]) continue;
        gateLearn(g, now, g->epoch, seq[k], result[k]);
        inv(g);
    }
}

TEST(answer_order_does_not_matter) {
    enum { N = 80, PHASES = 14, BROKEN = 6 };
    gate *g[3];
    uint64_t rng = 20261128, order_rng = 7, before_missing = 0;
    int64_t now = T0;
    int phase, k, nlost = 0;
    uint32_t i;

    for (k = 0; k < 3; k++) g[k] = scoreGate();
    for (phase = 0; phase < PHASES; phase++) {
        double score[N], u[N];
        int32_t result[N];
        int lost[N];
        uint64_t seq[3][N];

        for (i = 0; i < N; i++) {
            score[i] = gate_test_uniform(&rng);
            u[i] = gate_test_uniform(&rng);
            result[i] = gate_test_uniform(&rng) < (score[i] >= 0.5 ? 0.97 : 0.5);
            lost[i] = gate_test_uniform(&rng) < 0.04;
            /* The scorer is broken in one phase: every request is a spot
             * check and every guess is wrong. */
            if (phase == BROKEN) {
                u[i] = U_SPOT;
                result[i] = 0;
            }
        }
        /* The same requests, asked in the same state: same replies. */
        for (k = 0; k < 3; k++) {
            for (i = 0; i < N; i++) {
                gatePredictResult r = ask(g[k], now, u[i], score[i]);

                seq[k][i] = r.seq;
                CHECK(seq[k][i] == seq[0][i]);
            }
        }
        /* Every answer arrives before the oldest request of the batch is
         * GRACE old: proof waits for the answers that are still out. */
        for (k = 0; k < 3; k++)
            answerBatch(g[k], k, now + GRACE - 1, seq[k], result, lost, N, &order_rng);
        /* The lost answers time out. Whatever the order was, the proof is
         * the same down to the last bit, and so is the drift detector. (In
         * the broken phase the alarm is raised by the disagreement that
         * takes a block to 1000, which is another request in each gate;
         * that it is raised does not depend on the order, and what is left
         * is the same.) */
        now += TIMEOUT;
        for (k = 0; k < 3; k++) {
            gateMaintain(g[k], now);
            inv(g[k]);
            CHECK_EQ_INT(g[k]->npending, 0);
            CHECK_EQ_INT(g[k]->stats.revocations, phase >= BROKEN);
        }
        sameProof(g[0], g[1]);
        sameProof(g[0], g[2]);
        CHECK_EQ_INT(g[0]->stats.missing, g[1]->stats.missing);
        CHECK_EQ_INT(g[0]->stats.missing, g[2]->stats.missing);
        nlost += g[0]->stats.missing > before_missing;
        before_missing = g[0]->stats.missing;
        if (phase == BROKEN - 1) CHECK(g[0]->serving != NULL && servingOf(g[0])->audits >= 20);
    }
    /* The phases must have gone through lost answers, a promotion, the
     * drift alarm and a promotion of the generation after it; and never
     * through 32 lost answers in a row. */
    CHECK(nlost >= PHASES / 2);
    CHECK_EQ_INT(g[0]->stats.promotions, 2);
    CHECK(g[0]->serving != NULL && servingOf(g[0])->gen == 2);
    CHECK_EQ_INT(g[0]->blind, 0);
    for (k = 0; k < 3; k++) gateFree(g[k]);
}

/* The same for a classifier, with hand-made guesses for both generations so
 * that the three gates see the same guesses although their live models,
 * trained in three different orders, are no longer the same. */
TEST(answer_order_does_not_matter_classifier) {
    enum { N = 100, PHASES = 14 };
    static const double scores[] = {0.6, 0.8, 0.9, 0.97, 0.995, 1.0};
    gate *g[3];
    uint64_t rng = 424242, order_rng = 9;
    int64_t now = T0;
    int phase, k;
    uint32_t i;

    for (k = 0; k < 3; k++) {
        g[k] = classGate(2, 3);
        for (i = 0; i < 3; i++) train(g[k], now, &XA, 0);
    }
    for (phase = 0; phase < PHASES; phase++) {
        int32_t label[N], result[N];
        double sscore[N], cscore[N], u[N];
        int lost[N];
        uint64_t seq[3][N];

        for (i = 0; i < N; i++) {
            /* Both generations guess the same label; the serving one is
             * the less confident, so that a candidate can come to accept
             * more than it does. Half of the requests are spot checks. */
            label[i] = (int32_t)(gate_test_rand64(&rng) & 1);
            cscore[i] = scores[gate_test_rand64(&rng) % COUNTOF(scores)];
            sscore[i] = scores[gate_test_rand64(&rng) % 3];
            u[i] = 0.5 * gate_test_uniform(&rng);
            result[i] = gate_test_uniform(&rng) < (cscore[i] >= 0.9 ? 0.97 : 0.6)
                            ? label[i] : 1 - label[i];
            lost[i] = gate_test_uniform(&rng) < 0.04;
        }
        for (k = 0; k < 3; k++) {
            for (i = 0; i < N; i++) {
                gateEval ev = guesses(g[k], label[i], sscore[i], label[i], cscore[i]);
                gatePredictResult r = askGuess(g[k], now, u[i], &ev, &XA);

                seq[k][i] = r.seq;
                CHECK(seq[k][i] == seq[0][i]);
            }
        }
        for (k = 0; k < 3; k++)
            answerBatch(g[k], k, now + GRACE - 1, seq[k], result, lost, N, &order_rng);
        now += TIMEOUT;
        for (k = 0; k < 3; k++) {
            gateMaintain(g[k], now);
            inv(g[k]);
        }
        sameProof(g[0], g[1]);
        sameProof(g[0], g[2]);
        CHECK_EQ_INT(g[0]->stats.missing, g[1]->stats.missing);
        CHECK_EQ_INT(g[0]->stats.missing, g[2]->stats.missing);
    }
    /* The phases must have gone through a first snapshot, then a better
     * one, and lost answers. (No drift alarm: it wipes the live model, and
     * what the three then learn from the answers still to come, each in its
     * own order, is no longer the same.) */
    CHECK(g[0]->stats.promotions >= 2);
    CHECK(g[0]->stats.missing >= 1);
    for (k = 0; k < 3; k++) {
        CHECK_EQ_INT(g[k]->stats.revocations, 0);
        CHECK_EQ_INT(g[k]->blind, 0);
    }
    for (k = 0; k < 3; k++) gateFree(g[k]);
}

/* -------------------------------- Feedback ------------------------------- */

TEST(feedback_ids) {
    gate *g = classGate(2, 4);
    gatePredictResult r, r2;
    const gateObs *o;
    int64_t now = T0;
    uint32_t i;

    /* COLD: the request is recorded for training only. */
    r = askText(g, now, U_PLAIN, &XA);
    CHECK_EQ_INT(g->npending, 1);
    CHECK_EQ_INT(g->pending_bytes, GATE_OBS_BYTES(1));
    CHECK_EQ_INT(gateLearn(g, now, EPOCH + 1, r.seq, 0), 0); /* not this gate's id */
    CHECK_EQ_INT(gateLearn(g, now, EPOCH, 0, 0), 0);
    CHECK_EQ_INT(gateLearn(g, now, EPOCH, g->next_seq, 0), 0); /* not issued yet */
    CHECK_EQ_INT(gateLearn(g, now, EPOCH, g->next_seq + 1000, 0), 0);
    CHECK_EQ_INT(gateLearn(g, now, EPOCH, UINT64_MAX, 0), 0);
    CHECK_EQ_INT(g->stats.ignored, 5);
    CHECK_EQ_INT(g->stats.learned, 0);
    CHECK_EQ_INT(g->trainer.updates, 0);
    CHECK_EQ_INT(g->npending, 1);
    inv(g);
    /* Used once, and released at once: nothing else needs it. */
    CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
    CHECK_EQ_INT(g->trainer.updates, 1);
    CHECK_EQ_INT(g->npending, 0);
    CHECK_EQ_INT(g->pending_bytes, 0);
    CHECK(gateFindObservation(g, r.seq) == NULL);
    CHECK_EQ_INT(answer(g, now, r.seq, 0), 0);
    CHECK_EQ_INT(g->trainer.updates, 1);
    CHECK_EQ_INT(g->stats.learned, 1);
    CHECK_EQ_INT(g->stats.ignored, 6);

    /* Proof that has to wait for older proof: the model learns from the
     * answer right away and the clues are given back; the request itself
     * stays until its turn. A second answer is refused meanwhile. */
    for (i = 0; i < 3; i++) train(g, now, &XB, 1);
    CHECK(g->candidate != NULL);
    r = askText(g, now, U_PLAIN, &XA);
    r2 = askText(g, now, U_PLAIN, &XB);
    CHECK_EQ_INT(g->pending_bytes, 2 * GATE_OBS_BYTES(1));
    CHECK_EQ_INT(answer(g, now, r2.seq, 1), 1);
    CHECK_EQ_INT(g->trainer.updates, 5);
    CHECK_EQ_INT(g->pending_bytes, GATE_OBS_BYTES(1) + GATE_OBS_BYTES(0));
    o = gateFindObservation(g, r2.seq);
    CHECK(o != NULL && o->x.n == 0 && o->x.idx == NULL && o->x.val == NULL && o->result == 1);
    CHECK_EQ_INT(candidateOf(g)->samples, 0);
    CHECK_EQ_INT(answer(g, now, r2.seq, 1), 0);
    CHECK_EQ_INT(g->trainer.updates, 5);
    CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
    CHECK_EQ_INT(candidateOf(g)->samples, 2);
    CHECK_EQ_INT(g->npending, 0);
    CHECK_EQ_INT(g->pending_bytes, 0);
    CHECK_EQ_INT(answer(g, now, r.seq, 0), 0);

    /* The feedback window: an answer at the last millisecond is used, one
     * that arrives when the window has closed is not. */
    r = askText(g, now, U_PLAIN, &XA);
    CHECK_EQ_INT(answer(g, now + TIMEOUT - 1, r.seq, 0), 1);
    now += TIMEOUT;
    r = askText(g, now, U_PLAIN, &XA);
    CHECK_EQ_INT(answer(g, now + TIMEOUT, r.seq, 0), 0);
    CHECK_EQ_INT(g->stats.missing, 1);
    CHECK_EQ_INT(g->trainer.updates, 7);
    now += TIMEOUT;

    /* Ids from before a recovery or a reset are refused, under the old
     * epoch and under the new one. */
    r = askText(g, now, U_PLAIN, &XA);
    gateRecover(g, now, EPOCH2);
    inv(g);
    CHECK_EQ_INT(gateLearn(g, now, EPOCH, r.seq, 0), 0);
    CHECK_EQ_INT(gateLearn(g, now, EPOCH2, r.seq, 0), 0);
    r = askText(g, now, U_PLAIN, &XA);
    CHECK(r.seq != 0);
    gateReset(g, now, EPOCH3);
    inv(g);
    CHECK_EQ_INT(gateLearn(g, now, EPOCH2, r.seq, 0), 0);
    CHECK_EQ_INT(gateLearn(g, now, EPOCH3, r.seq, 0), 0);
    CHECK_EQ_INT(g->trainer.updates, 0);
    r = askText(g, now, U_PLAIN, &XA);
    CHECK_EQ_INT(gateLearn(g, now, EPOCH3, r.seq, 0), 1);
    gateFree(g);
}

/* ------------------------------ The grace rule ---------------------------
 * Proof is applied in request order. An unanswered request holds up the
 * answers behind it until it is GRACE old; then proof moves on: the request
 * is counted as a wrong guess for now (PROVISIONAL), leaves the queue and
 * keeps its id. */

TEST(grace_moves_on) {
    gate *g = scoreGate();
    gatePredictResult h, h2, h3, a;
    int64_t t = T0;

    /* Nothing answered waits: nothing moves, however old the head gets,
     * until its time is up. (0.5 is in the regions 0..3, 0.9 in 0..11.) */
    h = ask(g, t, U_PLAIN, 0.5);
    h2 = ask(g, t + 10, U_PLAIN, 0.9);
    CHECK_EQ_INT(gateUnanswered(g), 2);
    expectIdle(g, t + GRACE);
    expectIdle(g, t + GRACE + 300);
    expectIdle(g, t + TIMEOUT - 1);
    CHECK_EQ_INT(flagsOf(g, h.seq), GATE_OBS_CAND);
    CHECK_EQ_INT(flagsOf(g, h2.seq), GATE_OBS_CAND);
    CHECK_EQ_INT(g->nevidence, 2);
    CHECK_EQ_INT(candidateOf(g)->samples, 0);
    CHECK_EQ_INT(gateMaintenanceDue(g, t + TIMEOUT), 1);
    gateFree(g);

    /* An answer waits behind the head: proof moves on when the head is
     * GRACE old, to the millisecond. It is the age of the head that counts;
     * the answered request is 10 ms younger. */
    g = scoreGate();
    h = ask(g, t, U_PLAIN, 0.5);
    a = ask(g, t + 10, U_PLAIN, 0.9);
    CHECK_EQ_INT(answer(g, t + 20, a.seq, 1), 1);
    CHECK_EQ_INT(flagsOf(g, a.seq), GATE_OBS_CAND | GATE_OBS_RESOLVED);
    CHECK_EQ_INT(g->nanswered, 1);
    CHECK_EQ_INT(gateUnanswered(g), 1);
    CHECK_EQ_INT(candidateOf(g)->samples, 0);
    expectIdle(g, t + GRACE - 1);
    expectDue(g, t + GRACE);
    CHECK_EQ_INT(flagsOf(g, h.seq), GATE_OBS_CAND | GATE_OBS_PROVISIONAL);
    CHECK(gateFindObservation(g, a.seq) == NULL);
    /* The head as a wrong guess, then the answer. */
    CHECK_EQ_INT(candidateOf(g)->samples, 2);
    CHECK_EQ_INT(candidateOf(g)->errors, 1);
    expectRegions(g, candidateOf(g), 0, 0, 4, 2, 1);
    expectRegions(g, candidateOf(g), 0, 4, 12, 1, 0);
    expectRegions(g, candidateOf(g), 0, 12, T, 0, 0);
    /* The request still waits for its answer, outside the queue. */
    CHECK_EQ_INT(g->npending, 1);
    CHECK_EQ_INT(g->nanswered, 0);
    CHECK_EQ_INT(g->nevidence, 0);
    CHECK_EQ_INT(gateUnanswered(g), 1);
    CHECK(g->ehead == NULL && g->etail == NULL);
    CHECK_EQ_INT(g->stats.missing, 0);
    CHECK_EQ_INT(g->stats.dropped, 0);
    CHECK_EQ_INT(g->stats.learned, 1);
    CHECK_EQ_INT(g->missing_run, 0);
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    /* And nothing more happens to it until its time is up. */
    expectIdle(g, t + TIMEOUT - 1);
    CHECK_EQ_INT(gateMaintenanceDue(g, t + TIMEOUT), 1);
    gateFree(g);

    /* The answer arrives when the head is already older than GRACE: proof
     * moves on in that very call. */
    g = scoreGate();
    h = ask(g, t, U_PLAIN, 0.5);
    a = ask(g, t + 10, U_PLAIN, 0.9);
    expectIdle(g, t + 400);
    CHECK_EQ_INT(answer(g, t + 400, a.seq, 1), 1);
    CHECK_EQ_INT(flagsOf(g, h.seq), GATE_OBS_CAND | GATE_OBS_PROVISIONAL);
    CHECK_EQ_INT(candidateOf(g)->samples, 2);
    CHECK_EQ_INT(candidateOf(g)->errors, 1);
    CHECK_EQ_INT(g->npending, 1);
    gateFree(g);

    /* Only what is GRACE old is left behind: a younger unanswered request
     * becomes the head and holds the answer up in its turn. */
    g = scoreGate();
    h = ask(g, t, U_PLAIN, 0.9);
    h2 = ask(g, t + 100, U_PLAIN, 0.9);
    a = ask(g, t + 100, U_PLAIN, 0.9);
    CHECK_EQ_INT(answer(g, t + 100, a.seq, 1), 1);
    expectDue(g, t + GRACE);
    CHECK_EQ_INT(flagsOf(g, h.seq), GATE_OBS_CAND | GATE_OBS_PROVISIONAL);
    CHECK_EQ_INT(flagsOf(g, h2.seq), GATE_OBS_CAND);
    CHECK_EQ_INT(flagsOf(g, a.seq), GATE_OBS_CAND | GATE_OBS_RESOLVED);
    CHECK_EQ_INT(candidateOf(g)->samples, 1);
    CHECK_EQ_INT(candidateOf(g)->errors, 1);
    CHECK_EQ_INT(g->nanswered, 1);
    CHECK_EQ_INT(g->nevidence, 2);
    expectIdle(g, t + 100 + GRACE - 1);
    expectDue(g, t + 100 + GRACE);
    CHECK_EQ_INT(flagsOf(g, h2.seq), GATE_OBS_CAND | GATE_OBS_PROVISIONAL);
    CHECK_EQ_INT(candidateOf(g)->samples, 3);
    CHECK_EQ_INT(candidateOf(g)->errors, 2);
    CHECK_EQ_INT(g->npending, 2);
    CHECK_EQ_INT(gateUnanswered(g), 2);
    gateFree(g);

    /* Several late requests in a row are left behind in one call. */
    g = scoreGate();
    h = ask(g, t, U_PLAIN, 0.9);
    h2 = ask(g, t + 1, U_PLAIN, 0.9);
    h3 = ask(g, t + 2, U_PLAIN, 0.9);
    a = ask(g, t + 3, U_PLAIN, 0.9);
    CHECK_EQ_INT(answer(g, t + 3, a.seq, 1), 1);
    expectIdle(g, t + GRACE - 1);
    expectDue(g, t + GRACE + 2);
    CHECK_EQ_INT(flagsOf(g, h.seq), GATE_OBS_CAND | GATE_OBS_PROVISIONAL);
    CHECK_EQ_INT(flagsOf(g, h2.seq), GATE_OBS_CAND | GATE_OBS_PROVISIONAL);
    CHECK_EQ_INT(flagsOf(g, h3.seq), GATE_OBS_CAND | GATE_OBS_PROVISIONAL);
    CHECK_EQ_INT(candidateOf(g)->samples, 4);
    CHECK_EQ_INT(candidateOf(g)->errors, 3);
    CHECK_EQ_INT(g->npending, 3);
    CHECK_EQ_INT(g->nevidence, 0);
    gateFree(g);
}

/* GRACE is a quarter of the feedback timeout, rounded down, and at least a
 * millisecond. */
TEST(grace_is_a_quarter) {
    static const struct {
        int64_t timeout, grace;
    } cases[] = {{1, 1},         {3, 1},     {4, 1},         {7, 1},
                 {8, 2},         {1000, 250}, {1003, 250},   {60000, 15000},
                 {GATE_MAX_FEEDBACK_TIMEOUT_MS, 151200000}};
    size_t c;

    for (c = 0; c < COUNTOF(cases); c++) {
        const int64_t grace = cases[c].grace;
        gatePredictResult h, a;
        gateConfig cfg;
        gate *g;

        baseConfig(&cfg);
        cfg.feedback_timeout_ms = cases[c].timeout;
        g = create(&cfg);
        h = ask(g, T0, U_PLAIN, 0.9);
        a = ask(g, T0, U_PLAIN, 0.9);
        CHECK_EQ_INT(answer(g, T0, a.seq, 1), 1);
        expectIdle(g, T0 + grace - 1);
        expectDue(g, T0 + grace);
        CHECK_EQ_INT(candidateOf(g)->samples, 2);
        CHECK_EQ_INT(candidateOf(g)->errors, 1);
        if (grace < cases[c].timeout) {
            CHECK_EQ_INT(flagsOf(g, h.seq), GATE_OBS_CAND | GATE_OBS_PROVISIONAL);
            CHECK_EQ_INT(g->stats.missing, 0);
            CHECK_EQ_INT(g->npending, 1);
        } else {
            /* A timeout of one millisecond: its time is up at that moment. */
            CHECK(gateFindObservation(g, h.seq) == NULL);
            CHECK_EQ_INT(g->stats.missing, 1);
            CHECK_EQ_INT(g->npending, 0);
        }
        gateFree(g);
    }
}

/* GPREDICT without the walk over every waiting request that inv() makes:
 * for the tests that let thousands of them wait. */
static uint64_t quickAsk(gate *g, int64_t now, double u, double score) {
    gatePredictResult res;
    gateEval ev;

    gateMaintain(g, now);
    if (gateEvaluateFor(g, NULL, score, u, &ev) != GATE_OK ||
        gatePredict(g, now, u, &ev, NULL, &res) != GATE_OK)
        die("gatePredict failed");
    return res.seq;
}

/* 'n' requests of a score gate, each answered "right" at once. Returns how
 * many were recorded and had their answer used. */
static uint32_t quickFeed(gate *g, int64_t now, uint32_t n) {
    uint32_t i;

    for (i = 0; i < n; i++) {
        uint64_t seq = quickAsk(g, now, U_PLAIN, 0.9);

        if (seq == 0 || gateLearn(g, now, g->epoch, seq, 1) != 1) break;
    }
    return i;
}

/* Proof also moves on when more than GATE_MAX_WAITING answers wait behind
 * an unanswered request, whatever its age: everything here happens within
 * one millisecond. */
TEST(waiting_limit) {
    gate *g = scoreGate();
    uint64_t head, head2;
    int64_t t = T0;

    head = quickAsk(g, t, U_PLAIN, 0.9);
    CHECK_EQ_INT(quickFeed(g, t, GATE_MAX_WAITING), GATE_MAX_WAITING);
    inv(g);
    /* Exactly the limit: they wait. They do not make the gate full. */
    CHECK_EQ_INT(g->nanswered, GATE_MAX_WAITING);
    CHECK_EQ_INT(g->npending, GATE_MAX_WAITING + 1);
    CHECK_EQ_INT(gateUnanswered(g), 1);
    CHECK(!gateFull(g));
    CHECK_EQ_INT(candidateOf(g)->samples, 0);
    CHECK_EQ_INT(flagsOf(g, head), GATE_OBS_CAND);
    CHECK_EQ_INT(gateMaintenanceDue(g, t), 0);
    /* One more: proof moves on. The head as a wrong guess, then 16385
     * correct ones in request order: the 47th proved the thresholds 0..11
     * (at 48 answers), the snapshot was promoted and the rest went to it. */
    CHECK_EQ_INT(quickFeed(g, t, 1), 1);
    inv(g);
    CHECK_EQ_INT(flagsOf(g, head), GATE_OBS_CAND | GATE_OBS_PROVISIONAL);
    CHECK_EQ_INT(g->npending, 1);
    CHECK_EQ_INT(g->nanswered, 0);
    CHECK(g->serving != NULL && g->candidate == NULL);
    CHECK_EQ_INT(servingOf(g)->samples, GATE_MAX_WAITING + 2);
    CHECK_EQ_INT(servingOf(g)->errors, 1);
    CHECK_EQ_INT(servingOf(g)->regions[0].n, GATE_MAX_WAITING + 2);
    CHECK_EQ_INT(servingOf(g)->regions[0].errors, 1);
    CHECK_EQ_INT(servingOf(g)->regions[11].certified_at, 48);
    CHECK_EQ_INT(servingOf(g)->policy[0], 0);
    /* The head still has its id, and its answer still counts. */
    CHECK_EQ_INT(answer(g, t, head, 1), 1);
    CHECK_EQ_INT(servingOf(g)->errors, 0);
    CHECK_EQ_INT(servingOf(g)->regions[0].errors, 0);
    gateFree(g);

    /* Two unanswered requests in front: both are left behind by the same
     * answer, because the second one holds up as many as the first. */
    g = scoreGate();
    head = quickAsk(g, t, U_PLAIN, 0.9);
    head2 = quickAsk(g, t, U_PLAIN, 0.9);
    CHECK_EQ_INT(quickFeed(g, t, GATE_MAX_WAITING), GATE_MAX_WAITING);
    CHECK_EQ_INT(candidateOf(g)->samples, 0);
    CHECK_EQ_INT(quickFeed(g, t, 1), 1);
    inv(g);
    CHECK_EQ_INT(flagsOf(g, head), GATE_OBS_CAND | GATE_OBS_PROVISIONAL);
    CHECK_EQ_INT(flagsOf(g, head2), GATE_OBS_CAND | GATE_OBS_PROVISIONAL);
    CHECK_EQ_INT(g->npending, 2);
    CHECK(g->serving != NULL);
    CHECK_EQ_INT(servingOf(g)->samples, GATE_MAX_WAITING + 3);
    CHECK_EQ_INT(servingOf(g)->errors, 2);
    CHECK_EQ_INT(servingOf(g)->regions[11].certified_at, 59); /* 57 correct, 2 wrong */
    gateFree(g);

    /* A second unanswered request further back: once the ten answers in
     * front of it are applied it holds up no more than the limit, and it is
     * not GRACE old. Proof waits for it. */
    g = scoreGate();
    head = quickAsk(g, t, U_PLAIN, 0.9);
    CHECK_EQ_INT(quickFeed(g, t, 10), 10);
    head2 = quickAsk(g, t, U_PLAIN, 0.9);
    CHECK_EQ_INT(quickFeed(g, t, GATE_MAX_WAITING - 10), GATE_MAX_WAITING - 10);
    CHECK_EQ_INT(g->nanswered, GATE_MAX_WAITING);
    CHECK_EQ_INT(candidateOf(g)->samples, 0);
    CHECK_EQ_INT(quickFeed(g, t, 1), 1);
    inv(g);
    CHECK_EQ_INT(flagsOf(g, head), GATE_OBS_CAND | GATE_OBS_PROVISIONAL);
    CHECK_EQ_INT(flagsOf(g, head2), GATE_OBS_CAND);
    CHECK_EQ_INT(candidateOf(g)->samples, 11);
    CHECK_EQ_INT(candidateOf(g)->errors, 1);
    CHECK_EQ_INT(g->nanswered, GATE_MAX_WAITING - 9);
    CHECK_EQ_INT(g->npending, GATE_MAX_WAITING - 9 + 2);
    CHECK_EQ_INT(gateMaintenanceDue(g, t + GRACE - 1), 0);
    CHECK_EQ_INT(gateMaintenanceDue(g, t + GRACE), 1);
    gateFree(g);
}

/* ------------------------------- Late answers ----------------------------
 * The answer of a request that proof has moved on without takes the wrong
 * guess back where the recorded guess was right: from the generations that
 * counted it, if they are still there. */

/* The counts of two generations are the same and their wealth is the same
 * up to 'tol' (see the file comment). */
static void sameCounts(const gate *g, const gateGeneration *a, const gateGeneration *b,
                       double tol)
{
    uint32_t i;
    int j;

    CHECK_EQ_INT(a->gen, b->gen);
    CHECK_EQ_INT(a->samples, b->samples);
    CHECK_EQ_INT(a->errors, b->errors);
    CHECK_EQ_INT(a->audits, b->audits);
    CHECK_EQ_INT(a->audit_errors, b->audit_errors);
    CHECK(memcmp(a->policy, b->policy, sizeof(int32_t) * g->nrows) == 0);
    for (i = 0; i < g->nrows * T; i++) {
        CHECK_EQ_INT(a->regions[i].n, b->regions[i].n);
        CHECK_EQ_INT(a->regions[i].errors, b->regions[i].errors);
        for (j = 0; j < GATE_NBETS; j++)
            CHECK_NEAR(a->regions[i].logw[j], b->regions[i].logw[j], tol);
    }
}

TEST(late_answer_restores_the_proof) {
    enum { N = 36 };
    gate *late = scoreGate(), *prompt = scoreGate();
    gatePredictResult rl[N], rp[N];
    int64_t t = T0;
    uint32_t i;

    for (i = 0; i < N; i++) {
        rl[i] = ask(late, t, U_PLAIN, 0.9);
        rp[i] = ask(prompt, t, U_PLAIN, 0.9);
    }
    /* 'prompt': every answer in its turn. 35 correct answers prove the
     * thresholds 0..11. */
    for (i = 0; i < N; i++) CHECK_EQ_INT(answer(prompt, t, rp[i].seq, 1), 1);
    CHECK(prompt->serving != NULL);
    CHECK_EQ_INT(servingOf(prompt)->regions[0].certified_at, 35);

    /* 'late': the first answer is late and proof moves on without it. The
     * wrong guess comes first, in its turn: 35 correct answers after it
     * prove nothing (the wealth is 40, not 322). */
    for (i = 1; i < N; i++) CHECK_EQ_INT(answer(late, t + 5, rl[i].seq, 1), 1);
    CHECK_EQ_INT(candidateOf(late)->samples, 0);
    expectDue(late, t + GRACE);
    CHECK(late->serving == NULL);
    CHECK_EQ_INT(candidateOf(late)->samples, N);
    CHECK_EQ_INT(candidateOf(late)->errors, 1);
    expectRegions(late, candidateOf(late), 0, 0, 12, N, 1);
    CHECK_EQ_INT(candidateOf(late)->policy[0], -1);

    /* Its answer arrives: 36 correct answers. That is above the bar, so the
     * thresholds are proven by this answer, at the 36 answers they have
     * seen, and the snapshot is promoted now. */
    CHECK_EQ_INT(answer(late, t + GRACE + 50, rl[0].seq, 1), 1);
    CHECK(late->serving != NULL && late->candidate == NULL);
    CHECK_EQ_INT(servingOf(late)->promoted_ms, t + GRACE + 50);
    CHECK_EQ_INT(late->stats.promotions, 1);
    for (i = 0; i < T; i++) CHECK_EQ_INT(servingOf(late)->regions[i].certified_at, i < 12 ? N : 0);
    expectRegions(late, servingOf(late), 0, 0, 12, N, 0);

    /* What an answer in its turn would have given: the same counters,
     * exactly, and the same wealth up to rounding. Only the moment of the
     * proof is the later one. */
    sameCounts(late, servingOf(late), servingOf(prompt), 1e-12);
    CHECK_EQ_INT(late->npending, 0);
    CHECK_EQ_INT(late->stats.learned, prompt->stats.learned);
    CHECK_EQ_INT(late->stats.missing, 0);
    gateFree(late);
    gateFree(prompt);
}

/* The same on 1500 requests with scores, answers and latencies drawn at
 * random: one gate gets every answer at once, its twin up to 900 ms later
 * and out of order, so that proof moves on without most of them and each
 * one is set right when it arrives. 2% of the answers never arrive. The
 * expected counters are counted here, from the requests themselves. */
TEST(late_answers_restore_the_counters) {
    enum { N = 1500, STEP = 5 };
    static const double c[GATE_NBETS] = {0.1, 0.25, 0.5, 0.9};
    static double score[N];
    static int correct[N], lost[N], open[N];
    static int64_t due[N];
    static uint64_t seq[N];
    uint64_t rng = 20261129, nlost = 0, nwrong = 0;
    uint32_t i, j, t, first = 0, late = 0, taken_back = 0;
    gateConfig cfg;
    gate *g[2];
    int k;

    /* A gate that can prove nothing here (the wealth to reach is 6.4e13):
     * both stay CALIBRATING and every request is proof. */
    for (k = 0; k < 2; k++) {
        baseConfig(&cfg);
        cfg.delta = 1e-12;
        g[k] = create(&cfg);
    }
    for (i = 0; i < N; i++) {
        score[i] = gate_test_rand64(&rng) % 4 ? gate_test_uniform(&rng)
                                              : g[0]->grid[gate_test_rand64(&rng) % T];
        correct[i] = gate_test_uniform(&rng) < 0.8;
        lost[i] = gate_test_uniform(&rng) < 0.02;
        due[i] = T0 + STEP * (int64_t)i + (int64_t)(gate_test_rand64(&rng) % 900);
        open[i] = !lost[i];
        nlost += (uint64_t)lost[i];
        nwrong += (uint64_t)(lost[i] || !correct[i]);
    }
    for (i = 0; i <= N; i++) {
        /* The last round delivers what is still on its way. */
        int64_t now = i < N ? T0 + STEP * (int64_t)i : T0 + STEP * N + TIMEOUT;

        /* The answers that have arrived by now, in the order of arrival. */
        for (;;) {
            uint32_t best = N;

            while (first < i && !open[first]) first++;
            for (j = first; j < i; j++)
                if (open[j] && due[j] <= now && (best == N || due[j] < due[best])) best = j;
            if (best == N) break;
            if (flagsOf(g[1], seq[best]) & GATE_OBS_PROVISIONAL) {
                late++;
                taken_back += (uint32_t)correct[best];
            }
            CHECK_EQ_INT(gateLearn(g[1], due[best], EPOCH, seq[best], correct[best]), 1);
            inv(g[1]);
            open[best] = 0;
        }
        if (i == N) break;
        seq[i] = ask(g[1], now, U_PLAIN, score[i]).seq;
        CHECK(seq[i] != 0 && ask(g[0], now, U_PLAIN, score[i]).seq == seq[i]);
        if (!lost[i]) CHECK_EQ_INT(answer(g[0], now, seq[i], correct[i]), 1);
    }
    for (k = 0; k < 2; k++) {
        const gateGeneration *gen = candidateOf(g[k]);

        gateMaintain(g[k], T0 + STEP * N + 3 * TIMEOUT);
        inv(g[k]);
        CHECK(gateState(g[k]) == GATE_STATE_CALIBRATING && gen->gen == 1);
        CHECK_EQ_INT(g[k]->npending, 0);
        CHECK_EQ_INT(g[k]->stats.missing, nlost);
        CHECK_EQ_INT(g[k]->stats.learned, N - nlost);
        CHECK_EQ_INT(g[k]->stats.dropped, 0);
        CHECK_EQ_INT(g[k]->blind, 0);
        CHECK_EQ_INT(gen->samples, N);
        CHECK_EQ_INT(gen->errors, nwrong);
        for (t = 0; t < T; t++) {
            const gateRegion *reg = &gen->regions[t];
            uint64_t n = 0, e = 0;

            for (i = 0; i < N; i++) {
                if (!(score[i] >= g[k]->grid[t])) continue;
                n++;
                e += (uint64_t)(lost[i] || !correct[i]);
            }
            CHECK_EQ_INT(reg->n, n);
            CHECK_EQ_INT(reg->errors, e);
            CHECK_EQ_INT(reg->certified_at, 0);
            /* n - e gains of log(1 + c alpha / (1 - alpha)) and e losses
             * of log(1 - c), in whatever order: within 1e-9. */
            for (j = 0; j < GATE_NBETS; j++)
                CHECK_NEAR(reg->logw[j],
                           (double)(n - e) * log1p(c[j] * ALPHA / (1.0 - ALPHA)) +
                               (double)e * log1p(-c[j]),
                           1e-9);
        }
    }
    sameCounts(g[0], candidateOf(g[0]), candidateOf(g[1]), 1e-9);
    /* The walk must have been through what it is there for. */
    CHECK(nlost >= 10 && late >= 300 && taken_back >= 200);
    gateFree(g[0]);
    gateFree(g[1]);
}

/* A request that is proof for both generations: each takes its wrong guess
 * back if its own recorded guess was right. */
TEST(late_answer_both_generations) {
    static const struct {
        int32_t cand_label, result;
        int back_serv, back_cand; /* who takes the wrong guess back */
    } cases[] = {
        {0, 0, 1, 1}, /* both generations said label0, and it is label0 */
        {0, 1, 0, 0}, /* both were wrong: nothing is taken back */
        {1, 1, 0, 1}, /* only generation 2 (the candidate) was right */
        {1, 0, 1, 0}  /* only generation 1 (serving) was right */
    };
    const uint32_t both = GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_SERV | GATE_OBS_CAND;
    int64_t t = T0;
    size_t c;

    for (c = 0; c < COUNTOF(cases); c++) {
        /* What generation 1 guessed for the one request that is proof for
         * generation 2. */
        static const servGuess one = {0, 0.99, 1};
        gate *g = twoGenerations(t);
        uint32_t crow = (uint32_t)cases[c].cand_label;
        uint64_t kept_serv = (uint64_t)(1 - cases[c].back_serv);
        uint64_t kept_cand = (uint64_t)(1 - cases[c].back_cand);
        gatePredictResult late, held;
        gateEval ev;

        /* A spot check of an answer of generation 1 (label0 at 0.99: the
         * regions 0..19) that generation 2 would have answered with
         * 'cand_label' at 0.7 (the regions 0..2). */
        ev = guesses(g, 0, 0.99, cases[c].cand_label, 0.7);
        late = askGuess(g, t, U_SPOT, &ev, &XA);
        CHECK(late.action == GATE_ACTION_AUDIT);
        CHECK_EQ_INT(flagsOf(g, late.seq), both);
        /* A spot check of an answer: the first of label0, numbered now. */
        CHECK_EQ_INT(spotOf(g, late.seq), 1);
        expectDrift(&servingOf(g)->drift[0], 1, 0, 0, 0);
        /* Proof for generation 1 only (label1 has no threshold yet, so
         * every request for it is; 0.8 is in the regions 0..4), answered
         * at once: it waits. */
        ev = guesses(g, 1, 0.8, 1, 0.8);
        held = askGuess(g, t, U_PLAIN, &ev, &XB);
        CHECK_EQ_INT(flagsOf(g, held.seq), GATE_OBS_SERV);
        CHECK_EQ_INT(spotOf(g, held.seq), 0); /* nothing the gate would have answered */
        CHECK_EQ_INT(answer(g, t, held.seq, 1), 1);
        CHECK_EQ_INT(servingOf(g)->samples, 39);
        expectPaired(g, candidateOf(g), NULL, 0);

        /* Proof moves on: a wrong guess for both generations, for now. */
        expectDue(g, t + GRACE);
        CHECK_EQ_INT(flagsOf(g, late.seq), both | GATE_OBS_PROVISIONAL);
        CHECK_EQ_INT(candidateOf(g)->samples, 1);
        CHECK_EQ_INT(candidateOf(g)->errors, 1);
        /* Generation 1 put it at label0, 0.99: thresholds 0..19 of that row. */
        expectPaired(g, candidateOf(g), &one, 1);
        CHECK_EQ_INT(candidateOf(g)->paired[0 * T + 19], 1);
        CHECK_EQ_INT(candidateOf(g)->paired[0 * T + 20], 0);
        expectRegions(g, candidateOf(g), crow, 0, 3, 1, 1);
        expectRegions(g, candidateOf(g), crow, 3, T, 0, 0);
        expectRegions(g, candidateOf(g), 1 - crow, 0, T, 0, 0);
        CHECK_EQ_INT(servingOf(g)->samples, 41);
        CHECK_EQ_INT(servingOf(g)->errors, 1);
        expectRegions(g, servingOf(g), 0, 0, 20, 40, 1);
        expectRegions(g, servingOf(g), 1, 0, 5, 1, 0);
        CHECK_EQ_INT(servingOf(g)->audits, 0); /* not an answered spot check yet */
        /* To the drift detector it is a spot check like any other without
         * its answer: right until told otherwise. */
        expectDrift(&servingOf(g)->drift[0], 1, 0, 0, 0);
        expectDrift(&servingOf(g)->drift[1], 0, 0, 0, 0);

        /* The answer arrives. */
        CHECK_EQ_INT(answer(g, t + GRACE + 50, late.seq, cases[c].result), 1);
        CHECK(gateFindObservation(g, late.seq) == NULL);
        CHECK_EQ_INT(g->npending, 0);
        CHECK_EQ_INT(candidateOf(g)->samples, 1);
        CHECK_EQ_INT(candidateOf(g)->errors, kept_cand);
        expectPaired(g, candidateOf(g), &one, 1); /* a count of requests: right or wrong */
        expectRegions(g, candidateOf(g), crow, 0, 3, 1, kept_cand);
        expectRegions(g, candidateOf(g), crow, 3, T, 0, 0);
        CHECK_EQ_INT(servingOf(g)->samples, 41);
        CHECK_EQ_INT(servingOf(g)->errors, kept_serv);
        expectRegions(g, servingOf(g), 0, 0, 20, 40, kept_serv);
        expectRegions(g, servingOf(g), 0, 20, T, 0, 0);
        expectRegions(g, servingOf(g), 1, 0, 5, 1, 0);
        /* Now it is an answered spot check of generation 1, and a
         * disagreement of spot check 1 of label0 if generation 1 was wrong. */
        CHECK_EQ_INT(servingOf(g)->audits, 1);
        CHECK_EQ_INT(servingOf(g)->audit_errors, kept_serv);
        expectDrift(&servingOf(g)->drift[0], 1, kept_serv, (uint32_t)kept_serv, 0);
        expectDrift(&servingOf(g)->drift[1], 0, 0, 0, 0);
        CHECK_NEAR(gateDriftLogStat(&servingOf(g)->drift[0], &g->drift_bets[0], 0),
                   driftStat(ALPHA, 1, kept_serv), 1e-12);
        /* The candidate's detectors are not in use. */
        expectDrift(&candidateOf(g)->drift[0], 0, 0, 0, 0);
        expectDrift(&candidateOf(g)->drift[1], 0, 0, 0, 0);
        CHECK(servingOf(g)->gen == 1 && candidateOf(g)->gen == 2);
        CHECK_EQ_INT(g->trainer.updates, 5 + 39 + 2);
        CHECK_EQ_INT(g->stats.missing, 0);
        gateFree(g);
    }
}

/* A request asked while a generation was the candidate stays proof for it
 * after its promotion, and so does its late answer. */
TEST(late_answer_after_promotion) {
    enum { N = 57 };
    const uint32_t both = GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_SERV | GATE_OBS_CAND;
    gate *g[2]; /* 0: the first answer is late; 1: every answer in its turn */
    uint64_t seq[2][N];
    int64_t t = T0;
    gateEval ev;
    uint32_t i;
    int k;

    for (k = 0; k < 2; k++) {
        g[k] = twoGenerations(t);
        /* A spot check both generations would answer with label0 at 0.99,
         * then 56 that only generation 2 would answer: generation 1 says
         * label1, for which it has no threshold. */
        ev = guesses(g[k], 0, 0.99, 0, 0.99);
        seq[k][0] = askGuess(g[k], t, U_SPOT, &ev, &XA).seq;
        for (i = 1; i < N; i++) {
            ev = guesses(g[k], 1, 0.8, 0, 0.99);
            seq[k][i] = askGuess(g[k], t, U_SPOT, &ev, &XA).seq;
        }
        CHECK_EQ_INT(flagsOf(g[k], seq[k][0]), both);
        CHECK_EQ_INT(flagsOf(g[k], seq[k][N - 1]), GATE_OBS_AUDIT | GATE_OBS_SERV | GATE_OBS_CAND);
    }
    /* In its turn, all label0: generation 2 has its 44 correct answers at
     * the 44th and takes over (it answers 44 of its requests, generation 1
     * one of them); the other 13 are proof for it as the serving one. */
    for (i = 0; i < N; i++) {
        CHECK_EQ_INT(answer(g[1], t, seq[1][i], 0), 1);
        CHECK_EQ_INT(servingOf(g[1])->gen, i < 43 ? 1 : 2);
    }
    CHECK_EQ_INT(servingOf(g[1])->samples, N);
    CHECK_EQ_INT(servingOf(g[1])->regions[0].certified_at, 44);

    /* Late: proof moves on without the first answer. A wrong guess, then
     * 56 correct ones: the 56th proves the thresholds 0..19 of label0 and
     * generation 2 takes over then. */
    for (i = 1; i < N; i++) CHECK_EQ_INT(answer(g[0], t, seq[0][i], 0), 1);
    CHECK_EQ_INT(candidateOf(g[0])->samples, 0);
    expectDue(g[0], t + GRACE);
    CHECK(g[0]->serving != NULL && servingOf(g[0])->gen == 2);
    CHECK_EQ_INT(g[0]->stats.promotions, 2);
    CHECK_EQ_INT(servingOf(g[0])->samples, N);
    CHECK_EQ_INT(servingOf(g[0])->errors, 1);
    expectRegions(g[0], servingOf(g[0]), 0, 0, 20, N, 1);
    for (i = 0; i < T; i++)
        CHECK_EQ_INT(servingOf(g[0])->regions[i].certified_at, i < 20 ? N : 0);
    CHECK(g[0]->candidate != NULL && candidateOf(g[0])->gen == 3);
    CHECK_EQ_INT(flagsOf(g[0], seq[0][0]), both | GATE_OBS_PROVISIONAL);

    /* Its answer: generation 2 guessed right. It serves now, and it is the
     * generation that counted the wrong guess. */
    CHECK_EQ_INT(answer(g[0], t + GRACE + 50, seq[0][0], 0), 1);
    CHECK_EQ_INT(servingOf(g[0])->errors, 0);
    expectRegions(g[0], servingOf(g[0]), 0, 0, 20, N, 0);
    /* It was a spot check of an answer of generation 1, which is gone: no
     * audit of generation 2, and the number it carried is nothing to the
     * detectors of generation 2. Generation 3 never counted it. */
    CHECK_EQ_INT(servingOf(g[0])->audits, 0);
    expectDrift(&servingOf(g[0])->drift[0], 0, 0, 0, 0);
    expectDrift(&servingOf(g[0])->drift[1], 0, 0, 0, 0);
    CHECK(candidateOf(g[0])->samples == 0 && candidateOf(g[0])->errors == 0);
    /* The counters of the answers in their turn; the proof is dated later. */
    sameCounts(g[0], servingOf(g[0]), servingOf(g[1]), wealthTol(N));
    /* What generation 1 guessed for the requests of generation 2 was
     * cleared at the takeover, and neither the 13 requests after it nor the
     * late answer are requests of a snapshot under test. */
    for (k = 0; k < 2; k++) {
        expectPaired(g[k], servingOf(g[k]), NULL, 0);
        CHECK(g[k]->candidate != NULL && candidateOf(g[k])->gen == 3);
        expectPaired(g[k], candidateOf(g[k]), NULL, 0);
    }
    gateFree(g[0]);
    gateFree(g[1]);
}

/* A candidate that was dropped at its budget takes nothing back, and the
 * snapshot after it never counted the request. */
TEST(late_answer_after_budget) {
    const uint32_t both = GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_SERV | GATE_OBS_CAND;
    int64_t t = T0;
    gate *g = twoGenerations(t);
    gatePredictResult late, r;
    gateEval ev;
    uint32_t i;

    /* Generation 2 may consume 880 answers (two labels, 10 x 44 each: a
     * flawless threshold needs 44 correct answers to reach 1920). */
    CHECK_EQ_INT(candidateOf(g)->budget, 880);
    ev = guesses(g, 0, 0.99, 0, 0.99);
    late = askGuess(g, t, U_SPOT, &ev, &XA);
    /* Spot checks on which generation 1 is right (label0) and generation 2
     * is not (it says label1 at 0.6). The first one waits behind the open
     * request until proof moves on. */
    ev = guesses(g, 0, 0.99, 1, 0.6);
    r = askGuess(g, t, U_SPOT, &ev, &XA);
    CHECK_EQ_INT(answer(g, t, r.seq, 0), 1);
    expectDue(g, t + GRACE);
    CHECK_EQ_INT(candidateOf(g)->samples, 2);
    CHECK_EQ_INT(candidateOf(g)->errors, 2);
    for (i = 3; i <= 880; i++) {
        ev = guesses(g, 0, 0.99, 1, 0.6);
        r = askGuess(g, t + GRACE, U_SPOT, &ev, &XA);
        CHECK(r.action == GATE_ACTION_AUDIT);
        CHECK_EQ_INT(answer(g, t + GRACE, r.seq, 0), 1);
        /* At its 880th answer generation 2 is dropped; generation 3
         * follows at once. */
        CHECK_EQ_INT(candidateOf(g)->gen, i < 880 ? 2 : 3);
    }
    CHECK_EQ_INT(g->stats.retired, 1);
    CHECK_EQ_INT(g->generations, 3);
    CHECK_EQ_INT(candidateOf(g)->samples, 0);
    CHECK_EQ_INT(candidateOf(g)->budget, 960); /* 10 x 48 each to reach 3840 */
    expectPaired(g, candidateOf(g), NULL, 0);
    /* Generation 1 served all along: the open request as a wrong guess and
     * 879 spot checks it got right. */
    CHECK_EQ_INT(servingOf(g)->gen, 1);
    CHECK_EQ_INT(servingOf(g)->samples, 39 + 880);
    CHECK_EQ_INT(servingOf(g)->errors, 1);
    CHECK_EQ_INT(servingOf(g)->audits, 879);
    expectRegions(g, servingOf(g), 0, 0, 20, 919, 1);
    CHECK_EQ_INT(flagsOf(g, late.seq), both | GATE_OBS_PROVISIONAL);
    /* 880 spot checks of label0 were numbered, none disagreed. */
    CHECK_EQ_INT(servingOf(g)->drift[0].asked, 880);
    CHECK_EQ_INT(servingOf(g)->drift[0].errors, 0);
    CHECK_EQ_INT(spotOf(g, late.seq), 1);

    /* Its answer: label0. Generation 2 was right too, but it is gone, and
     * generation 3 never counted this request: only generation 1 takes its
     * wrong guess back. */
    CHECK_EQ_INT(answer(g, t + GRACE + 50, late.seq, 0), 1);
    CHECK_EQ_INT(servingOf(g)->errors, 0);
    expectRegions(g, servingOf(g), 0, 0, 20, 919, 0);
    CHECK_EQ_INT(servingOf(g)->audits, 880);
    CHECK_EQ_INT(servingOf(g)->audit_errors, 0);
    CHECK(candidateOf(g)->gen == 3 && candidateOf(g)->samples == 0 && candidateOf(g)->errors == 0);
    for (i = 0; i < 2 * T; i++) CHECK_EQ_INT(candidateOf(g)->regions[i].n, 0);
    expectPaired(g, candidateOf(g), NULL, 0);
    CHECK_EQ_INT(candidateOf(g)->missed, 0);
    CHECK_EQ_INT(g->stats.retired, 1);
    CHECK_EQ_INT(g->npending, 0);
    gateFree(g);

    /* Nothing serving: the first snapshot is dropped at its 780th answer.
     * The late answer has no generation left to tell; the model learns
     * from it all the same. */
    g = trainedGate(800);
    CHECK_EQ_INT(candidateOf(g)->budget, 780);
    late = askText(g, t, U_PLAIN, &XA);
    r = askText(g, t, U_PLAIN, &XA);
    CHECK_EQ_INT(answer(g, t, r.seq, 1), 1);
    expectDue(g, t + GRACE);
    CHECK_EQ_INT(candidateOf(g)->samples, 2);
    for (i = 3; i <= 780; i++) {
        r = askText(g, t + GRACE, U_PLAIN, &XA);
        CHECK_EQ_INT(answer(g, t + GRACE, r.seq, 1), 1);
        CHECK_EQ_INT(g->candidate != NULL, i < 780);
    }
    CHECK(gateState(g) == GATE_STATE_COLD);
    CHECK_EQ_INT(g->stats.retired, 1);
    CHECK_EQ_INT(flagsOf(g, late.seq), GATE_OBS_CAND | GATE_OBS_PROVISIONAL);
    CHECK_EQ_INT(g->trainer.updates, 800 + 779);
    CHECK_EQ_INT(answer(g, t + GRACE + 50, late.seq, 0), 1);
    CHECK_EQ_INT(g->trainer.updates, 800 + 780);
    CHECK(g->candidate == NULL && g->serving == NULL); /* 20 examples short of the next one */
    CHECK_EQ_INT(g->npending, 0);
    CHECK_EQ_INT(g->stats.dropped, 0);
    CHECK_EQ_INT(g->stats.missing, 0);
    gateFree(g);
}

TEST(late_answer_after_drift_alarm) {
    const uint32_t spot = GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_SERV;
    int64_t t = T0;
    int answered;

    for (answered = 0; answered < 2; answered++) {
        gate *g = activeScoreGate(t);
        gatePredictResult late = ask(g, t, U_SPOT, 0.9), r = ask(g, t, U_SPOT, 0.9);
        uint32_t i;

        /* Proof moves on without the first of two spot checks. */
        CHECK_EQ_INT(answer(g, t, r.seq, 1), 1);
        expectDue(g, t + GRACE);
        CHECK_EQ_INT(flagsOf(g, late.seq), spot | GATE_OBS_PROVISIONAL);
        CHECK_EQ_INT(servingOf(g)->errors, 1);
        /* Wrong spot checks until the alarm. The block holds two that do
         * not disagree (the open one, which counts as right to the drift
         * detector whatever the proof made of it, and the one that was
         * right): the ninth disagreement (stat(11, 9) = 2604.4; eight give
         * 892.1). Generation 1 is gone, generation 2 is under test. The
         * open request is proof for nothing any more, but it still has its
         * id. */
        CHECK_NEAR(exp(driftStat(ALPHA, 11, 9)), 2604.375, 1e-9);
        CHECK_NEAR(exp(driftStat(ALPHA, 10, 8)), 892.125, 1e-9);
        for (i = 1; i <= 9; i++) {
            r = ask(g, t + GRACE, U_SPOT, 0.9);
            CHECK_EQ_INT(answer(g, t + GRACE, r.seq, 0), 1);
            CHECK_EQ_INT(g->serving != NULL, i < 9);
        }
        CHECK(g->candidate != NULL && candidateOf(g)->gen == 2);
        CHECK_EQ_INT(g->stats.revocations, 1);
        CHECK_EQ_INT(flagsOf(g, late.seq), spot | GATE_OBS_PROVISIONAL);
        CHECK_EQ_INT(g->npending, 1);
        CHECK_EQ_INT(g->stats.dropped, 0);
        if (answered) {
            /* Its answer is used, and changes no proof. */
            CHECK_EQ_INT(answer(g, t + GRACE + 50, late.seq, 1), 1);
            CHECK_EQ_INT(g->stats.learned, 35 + 1 + 9 + 1);
            CHECK_EQ_INT(g->stats.dropped, 0);
        } else {
            /* Without one it is forgotten when its time is up. That is not
             * a missing answer: there is nothing it could be an error for. */
            expectIdle(g, t + TIMEOUT - 1);
            expectDue(g, t + TIMEOUT);
            CHECK_EQ_INT(g->stats.dropped, 1);
            CHECK_EQ_INT(answer(g, t + TIMEOUT, late.seq, 1), 0);
        }
        CHECK_EQ_INT(g->stats.missing, 0);
        CHECK_EQ_INT(g->missing_run, 0);
        CHECK_EQ_INT(g->npending, 0);
        CHECK(candidateOf(g)->gen == 2 && candidateOf(g)->samples == 0 &&
              candidateOf(g)->errors == 0 && candidateOf(g)->regions[0].n == 0);
        gateFree(g);
    }
}

TEST(late_answer_after_reset_and_recover) {
    int64_t t = T0;
    int reset;

    for (reset = 0; reset < 2; reset++) {
        gate *g = scoreGate();
        gatePredictResult late = ask(g, t, U_PLAIN, 0.9), r = ask(g, t, U_PLAIN, 0.9);

        CHECK_EQ_INT(answer(g, t, r.seq, 1), 1);
        expectDue(g, t + GRACE);
        CHECK_EQ_INT(flagsOf(g, late.seq), GATE_OBS_CAND | GATE_OBS_PROVISIONAL);
        if (reset) gateReset(g, t + GRACE, EPOCH2); else gateRecover(g, t + GRACE, EPOCH2);
        inv(g);
        /* The request went with everything that waited: its id is refused,
         * under the old epoch and under the new one. */
        CHECK_EQ_INT(g->npending, 0);
        CHECK_EQ_INT(gateLearn(g, t + GRACE, EPOCH, late.seq, 1), 0);
        CHECK_EQ_INT(gateLearn(g, t + GRACE, EPOCH2, late.seq, 1), 0);
        inv(g);
        CHECK_EQ_INT(g->stats.ignored, 2);
        CHECK_EQ_INT(g->stats.learned, reset ? 0 : 1);
        CHECK(g->candidate != NULL && candidateOf(g)->gen == 2);
        CHECK(candidateOf(g)->samples == 0 && candidateOf(g)->errors == 0);
        gateFree(g);
    }
}

TEST(late_answer_wrong) {
    gate *g = scoreGate();
    gatePredictResult late, r;
    gateRegion before[T];
    int64_t t = T0;

    late = ask(g, t, U_PLAIN, 0.9);
    r = ask(g, t, U_PLAIN, 0.9);
    CHECK_EQ_INT(answer(g, t, r.seq, 1), 1);
    expectDue(g, t + GRACE);
    memcpy(before, candidateOf(g)->regions, sizeof(before));
    /* The late answer says the guess was wrong: that is what it was counted
     * as. Nothing is taken back; not a bit of the wealth moves. */
    CHECK_EQ_INT(answer(g, t + GRACE + 50, late.seq, 0), 1);
    CHECK(memcmp(before, candidateOf(g)->regions, sizeof(before)) == 0);
    CHECK_EQ_INT(candidateOf(g)->samples, 2);
    CHECK_EQ_INT(candidateOf(g)->errors, 1);
    CHECK_EQ_INT(g->stats.learned, 2);
    CHECK_EQ_INT(g->stats.missing, 0);
    CHECK_EQ_INT(g->npending, 0);
    /* The request is done with: nothing is left to time out, and a second
     * answer is refused. */
    expectIdle(g, t + 3 * TIMEOUT);
    CHECK_EQ_INT(answer(g, t + 3 * TIMEOUT, late.seq, 1), 0);
    CHECK_EQ_INT(candidateOf(g)->errors, 1);
    CHECK_EQ_INT(g->stats.missing, 0);
    gateFree(g);

    /* A classifier learns from a late answer like from any other, and the
     * request keeps its clues until then. */
    g = trainedGate(12);
    late = askText(g, t, U_PLAIN, &XA);
    r = askText(g, t, U_PLAIN, &XA);
    CHECK_EQ_INT(answer(g, t, r.seq, 0), 1);
    expectDue(g, t + GRACE);
    CHECK_EQ_INT(g->trainer.updates, 13);
    CHECK_EQ_INT(g->pending_bytes, GATE_OBS_BYTES(1));
    CHECK_EQ_INT(answer(g, t + GRACE + 50, late.seq, 1), 1); /* label1: the guess was label0 */
    CHECK_EQ_INT(g->trainer.updates, 14);
    CHECK_EQ_INT(candidateOf(g)->samples, 2);
    CHECK_EQ_INT(candidateOf(g)->errors, 1);
    CHECK_EQ_INT(g->pending_bytes, 0);
    gateFree(g);
}

/* The drift detector hears a spot check the moment its answer arrives. It
 * is an alarm, not proof: it does not wait for the answers of older
 * requests, and one open spot check cannot hide the failed ones behind it.
 * What it makes of the answers does not depend on when they arrive: a spot
 * check has the number it was asked under and counts as right until its
 * answer says otherwise. */
TEST(drift_hears_answers_as_they_arrive) {
    const uint32_t spot = GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_SERV;
    int64_t t = T0;
    gate *g = activeScoreGate(t);
    gatePredictResult open = ask(g, t, U_SPOT, 0.9), late, r;
    uint32_t i;
    int right;

    /* The detector by itself (file comment): with a spot check in the
     * block that has no answer, seven disagreements leave it at stat(8, 7)
     * = 594.75 and the eighth raises the alarm. */
    CHECK_EQ_INT(alarmAfter(ALPHA, "oxxxxxxx"), 0);
    CHECK_EQ_INT(alarmAfter(ALPHA, "oxxxxxxxx"), 9);

    /* Seven wrong spot checks behind an open one. As proof they wait for
     * it; the detector has heard each of them, under its number. */
    CHECK_EQ_INT(spotOf(g, open.seq), 1);
    for (i = 1; i <= 7; i++) {
        r = ask(g, t, U_SPOT, 0.9);
        CHECK_EQ_INT(spotOf(g, r.seq), i + 1);
        CHECK_EQ_INT(answer(g, t, r.seq, 0), 1);
        CHECK_EQ_INT(flagsOf(g, r.seq), spot | GATE_OBS_RESOLVED);
        CHECK_EQ_INT(servingOf(g)->audits, i);
        CHECK_EQ_INT(servingOf(g)->audit_errors, i);
        expectDrift(&servingOf(g)->drift[0], i + 1, i, i, 0);
        CHECK_NEAR(gateDriftLogStat(&servingOf(g)->drift[0], &g->drift_bets[0], 0),
                   driftStat(ALPHA, i + 1, i), 1e-12);
    }
    CHECK_EQ_INT(servingOf(g)->samples, 35);
    CHECK_EQ_INT(servingOf(g)->errors, 0);
    CHECK_EQ_INT(g->nanswered, 7);
    CHECK_NEAR(gateDriftLogStat(&servingOf(g)->drift[0], &g->drift_bets[0], 0), log(594.75), 1e-12);
    CHECK(ask(g, t, U_PLAIN, 0.9).action == GATE_ACTION_ACCEPT);
    /* The eighth raises the alarm (stat(9, 8) = 1736.25), with all eight
     * still waiting for their turn as proof. Everything that waited was
     * proof for the generation that is gone: the answered ones are done
     * with, the open one is forgotten. */
    r = ask(g, t, U_SPOT, 0.9);
    CHECK_EQ_INT(answer(g, t, r.seq, 0), 1);
    CHECK(g->serving == NULL);
    CHECK_EQ_INT(g->stats.revocations, 1);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 2 && candidateOf(g)->samples == 0);
    CHECK_EQ_INT(g->npending, 0);
    CHECK_EQ_INT(g->nanswered, 0);
    CHECK_EQ_INT(g->stats.dropped, 1);
    CHECK_EQ_INT(g->stats.learned, 35 + 8);
    CHECK_EQ_INT(g->blind, 0);
    CHECK_EQ_INT(answer(g, t, open.seq, 1), 0);
    CHECK(ask(g, t, U_PLAIN, 0.9).action == GATE_ACTION_ABSTAIN);
    gateFree(g);

    /* When an answer arrives does not matter. A spot check asked before
     * six wrong ones is answered after them, and after proof has moved on
     * without it. Until then the detector holds stat(7, 6) = 206.25, as if
     * it had been right. If that is what its answer says, nothing changes.
     * If it disagrees too, it is the seventh of seven, stat(7, 7) = 1157.5:
     * the alarm, as if the answer had come first. */
    for (right = 0; right < 2; right++) {
        g = activeScoreGate(t);
        late = ask(g, t, U_SPOT, 0.9);
        for (i = 0; i < 6; i++) {
            r = ask(g, t, U_SPOT, 0.9);
            CHECK_EQ_INT(answer(g, t, r.seq, 0), 1);
        }
        /* Proof moves on. That tells the detector nothing: the open spot
         * check is a wrong guess to the proof only. */
        expectDue(g, t + GRACE);
        CHECK(g->serving != NULL);
        CHECK_EQ_INT(servingOf(g)->samples, 35 + 7);
        CHECK_EQ_INT(servingOf(g)->errors, 7);
        CHECK_EQ_INT(servingOf(g)->audits, 6);
        CHECK_EQ_INT(servingOf(g)->audit_errors, 6);
        expectDrift(&servingOf(g)->drift[0], 7, 6, 6, 0);
        CHECK_NEAR(gateDriftLogStat(&servingOf(g)->drift[0], &g->drift_bets[0], 0), log(206.25),
                   1e-12);
        CHECK_NEAR(gateDriftLogStat(&servingOf(g)->drift[0], &g->drift_bets[0], 1), log(206.25),
                   1e-12);
        CHECK_EQ_INT(spotOf(g, late.seq), 1);
        CHECK_EQ_INT(answer(g, t + GRACE + 50, late.seq, right), 1);
        if (right) {
            CHECK(g->serving != NULL);
            CHECK_EQ_INT(servingOf(g)->errors, 6);
            CHECK_EQ_INT(servingOf(g)->audits, 7);
            CHECK_EQ_INT(servingOf(g)->audit_errors, 6);
            expectDrift(&servingOf(g)->drift[0], 7, 6, 6, 0);
            CHECK_NEAR(gateDriftLogStat(&servingOf(g)->drift[0], &g->drift_bets[0], 0),
                       log(206.25), 1e-12);
            CHECK_EQ_INT(g->stats.revocations, 0);
        } else {
            /* The seventh wrong spot check of seven. */
            CHECK(g->serving == NULL);
            CHECK_EQ_INT(g->stats.revocations, 1);
            CHECK(g->candidate != NULL && candidateOf(g)->gen == 2);
        }
        gateFree(g);
    }

    /* An answered spot check is heard once: not again when its turn as
     * proof comes. */
    g = activeScoreGate(t);
    open = ask(g, t, U_SPOT, 0.9);
    r = ask(g, t, U_SPOT, 0.9);
    CHECK_EQ_INT(answer(g, t, r.seq, 0), 1);
    CHECK_EQ_INT(servingOf(g)->audits, 1);
    CHECK_EQ_INT(servingOf(g)->samples, 35);
    expectDrift(&servingOf(g)->drift[0], 2, 1, 1, 0);
    CHECK_EQ_INT(answer(g, t, open.seq, 1), 1);
    CHECK_EQ_INT(servingOf(g)->samples, 37);
    CHECK_EQ_INT(servingOf(g)->errors, 1);
    CHECK_EQ_INT(servingOf(g)->audits, 2);
    CHECK_EQ_INT(servingOf(g)->audit_errors, 1);
    expectDrift(&servingOf(g)->drift[0], 2, 1, 1, 0);
    gateFree(g);
}

/* A late answer can be the one that proves a threshold: here of the
 * generation that serves, whose policy moves down by it. */
TEST(late_answer_proves_a_threshold) {
    gate *g = scoreGate();
    gatePredictResult late, r;
    int64_t t = T0;
    uint32_t i;

    /* A wrong answer at 0.5 puts the thresholds 0..3 in debt; 35 correct
     * ones at 0.9 prove 4..11 and the gate answers from threshold 4. With
     * one wrong answer the thresholds 0..3 need 47 correct ones. */
    feed(g, t, U_PLAIN, 0.5, 0);
    for (i = 0; i < 35; i++) feed(g, t, U_PLAIN, 0.9, 1);
    CHECK(g->serving != NULL && servingOf(g)->policy[0] == 4);
    /* Twelve spot checks, all right; the answer of the first is late. */
    late = ask(g, t, U_SPOT, 0.9);
    for (i = 0; i < 11; i++) {
        r = ask(g, t, U_SPOT, 0.9);
        CHECK_EQ_INT(answer(g, t, r.seq, 1), 1);
    }
    expectDue(g, t + GRACE);
    /* 46 correct answers and two wrong guesses: far from the bar. */
    expectRegions(g, servingOf(g), 0, 0, 4, 48, 2);
    expectRegions(g, servingOf(g), 0, 4, 12, 47, 1);
    CHECK_EQ_INT(servingOf(g)->policy[0], 4);
    CHECK_EQ_INT(servingOf(g)->audits, 11);
    r = ask(g, t + GRACE, U_PLAIN, 0.3);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq == 0);
    /* The late answer makes it 47 and one: proven, by this answer, at the
     * 48 answers the thresholds have seen. The gate answers from 0. */
    CHECK_EQ_INT(answer(g, t + GRACE + 50, late.seq, 1), 1);
    expectRegions(g, servingOf(g), 0, 0, 4, 48, 1);
    expectRegions(g, servingOf(g), 0, 4, 12, 47, 0);
    for (i = 0; i < 12; i++) CHECK_EQ_INT(servingOf(g)->regions[i].certified_at, i < 4 ? 48 : 35);
    CHECK_EQ_INT(servingOf(g)->policy[0], 0);
    CHECK(gateRowThreshold(g, 0) == 0.0);
    CHECK_EQ_INT(servingOf(g)->audits, 12);
    CHECK_EQ_INT(g->stats.promotions, 1);
    r = ask(g, t + GRACE + 50, U_PLAIN, 0.3);
    CHECK(r.action == GATE_ACTION_ACCEPT);
    gateFree(g);
}

/* ...or the one by which a candidate takes over. */
TEST(late_answer_triggers_a_promotion) {
    int64_t t = T0;
    int would;

    for (would = 0; would < 2; would++) {
        gate *g = twoGenerations(t);
        gatePredictResult late, r;
        gateEval ev;
        uint32_t i;

        /* 44 spot checks that generation 2 gets right (label0 at 0.99);
         * the answer of the first is late. would = 0: generation 1 says
         * label1, which it does not answer. would = 1: generation 1
         * answers them too. */
        memset(&late, 0, sizeof(late));
        for (i = 0; i < 44; i++) {
            ev = guesses(g, would ? 0 : 1, would ? 0.99 : 0.8, 0, 0.99);
            r = askGuess(g, t, U_SPOT, &ev, &XA);
            if (i == 0) late = r; else CHECK_EQ_INT(answer(g, t, r.seq, 0), 1);
        }
        expectDue(g, t + GRACE);
        /* 43 correct answers and a wrong guess: nothing is proven (it
         * would take 56). */
        CHECK(servingOf(g)->gen == 1 && candidateOf(g)->gen == 2);
        CHECK_EQ_INT(candidateOf(g)->samples, 44);
        CHECK_EQ_INT(candidateOf(g)->errors, 1);
        /* Where generation 1 put the 44: at label0, 0.99, inside its policy
         * (it answers label0 from 0.5), or at label1, 0.8, where it has no
         * threshold at all. */
        {
            servGuess where = {would ? 0 : 1, would ? 0.99 : 0.8, 44};

            expectPaired(g, candidateOf(g), &where, 1);
            CHECK_EQ_INT(candidateOf(g)->paired[(would ? 0 : 1) * T + 0], 44);
        }
        CHECK_EQ_INT(candidateOf(g)->policy[0], -1);
        /* The late answer makes it 44 correct ones: the thresholds 0..19
         * of generation 2 are proven by it. */
        CHECK_EQ_INT(answer(g, t + GRACE + 50, late.seq, 0), 1);
        if (!would) {
            /* Generation 2 answers 44 requests where generation 1 answered
             * none: it takes over, now. */
            CHECK_EQ_INT(servingOf(g)->gen, 2);
            CHECK_EQ_INT(g->stats.promotions, 2);
            CHECK_EQ_INT(servingOf(g)->promoted_ms, t + GRACE + 50);
            CHECK_EQ_INT(servingOf(g)->samples, 44);
            CHECK_EQ_INT(servingOf(g)->errors, 0);
            CHECK_EQ_INT(servingOf(g)->policy[0], 0);
            CHECK_EQ_INT(servingOf(g)->regions[0].certified_at, 44);
            CHECK_EQ_INT(servingOf(g)->audits, 0);
            expectPaired(g, servingOf(g), NULL, 0); /* zeroed when it took over */
            CHECK(g->candidate != NULL && candidateOf(g)->gen == 3);
        } else {
            /* Generation 1 answered the same 44: level is not more.
             * Generation 2 has its thresholds and waits. */
            CHECK(servingOf(g)->gen == 1 && candidateOf(g)->gen == 2);
            CHECK_EQ_INT(g->stats.promotions, 1);
            CHECK_EQ_INT(candidateOf(g)->policy[0], 0);
            CHECK_EQ_INT(candidateOf(g)->regions[0].certified_at, 44);
            CHECK_EQ_INT(candidateOf(g)->errors, 0);
            /* Generation 1 was right as well. */
            CHECK_EQ_INT(servingOf(g)->errors, 0);
            CHECK_EQ_INT(servingOf(g)->audits, 44);
            CHECK_EQ_INT(servingOf(g)->audit_errors, 0);
        }
        gateFree(g);
    }
}

/* ---------------------------------- Timeouts -----------------------------
 * When the feedback timeout of a request expires its id is forgotten. If
 * it was proof, it ends as a missing answer: a wrong guess for good. */

TEST(timeouts_end_once) {
    gate *g = scoreGate();
    gatePredictResult passed, held, head;
    int64_t t = T0;

    passed = ask(g, t, U_PLAIN, 0.9);
    held = ask(g, t + 5, U_PLAIN, 0.9);
    CHECK_EQ_INT(answer(g, t + 5, held.seq, 1), 1);
    /* Asked later, when proof has moved on; nothing answered waits behind
     * it, so it stays the head of the queue. */
    head = ask(g, t + 400, U_PLAIN, 0.9);
    CHECK_EQ_INT(flagsOf(g, passed.seq), GATE_OBS_CAND | GATE_OBS_PROVISIONAL);
    CHECK_EQ_INT(flagsOf(g, head.seq), GATE_OBS_CAND);
    CHECK_EQ_INT(candidateOf(g)->samples, 2);
    CHECK_EQ_INT(candidateOf(g)->errors, 1);

    /* The feedback timeout of the request proof moved on without: it was
     * counted as a wrong guess already, now it is a missing answer. Once.
     * An answer arrived after it was asked (that is why proof moved on):
     * a lost answer, which is no part of a run of missing ones. */
    expectIdle(g, t + TIMEOUT - 1);
    expectDue(g, t + TIMEOUT);
    CHECK(gateFindObservation(g, passed.seq) == NULL);
    CHECK_EQ_INT(g->stats.missing, 1);
    CHECK_EQ_INT(g->answer_mark, 3);
    CHECK_EQ_INT(g->missing_run, 0);
    CHECK_EQ_INT(candidateOf(g)->samples, 2);
    CHECK_EQ_INT(candidateOf(g)->errors, 1);
    expectRegions(g, candidateOf(g), 0, 0, 12, 2, 1);
    CHECK_EQ_INT(answer(g, t + TIMEOUT, passed.seq, 1), 0); /* too late */
    CHECK_EQ_INT(candidateOf(g)->errors, 1);

    /* The feedback timeout of the head of the queue: a wrong guess and a
     * missing answer at the same moment. Once. No answer has arrived since
     * this one was asked: the first of a run. */
    expectIdle(g, t + 400 + TIMEOUT - 1);
    expectDue(g, t + 400 + TIMEOUT);
    CHECK_EQ_INT(g->stats.missing, 2);
    CHECK_EQ_INT(g->missing_run, 1);
    CHECK_EQ_INT(candidateOf(g)->samples, 3);
    CHECK_EQ_INT(candidateOf(g)->errors, 2);
    expectRegions(g, candidateOf(g), 0, 0, 12, 3, 2);
    CHECK_EQ_INT(answer(g, t + 400 + TIMEOUT, head.seq, 1), 0);
    CHECK_EQ_INT(g->npending, 0);
    expectIdle(g, t + 10 * TIMEOUT);
    CHECK_EQ_INT(g->stats.missing, 2);
    CHECK_EQ_INT(g->stats.dropped, 0);
    CHECK_EQ_INT(g->stats.learned, 1);
    CHECK_EQ_INT(g->stats.ignored, 2);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 1);
    gateFree(g);

    /* An answer at the last millisecond is used: by a request in the
     * queue... */
    g = scoreGate();
    head = ask(g, t, U_PLAIN, 0.9);
    CHECK_EQ_INT(answer(g, t + TIMEOUT - 1, head.seq, 1), 1);
    CHECK_EQ_INT(candidateOf(g)->samples, 1);
    CHECK_EQ_INT(candidateOf(g)->errors, 0);
    /* ...and by one that proof has moved on without. */
    passed = ask(g, t + TIMEOUT, U_PLAIN, 0.9);
    held = ask(g, t + TIMEOUT, U_PLAIN, 0.9);
    CHECK_EQ_INT(answer(g, t + TIMEOUT, held.seq, 1), 1);
    expectDue(g, t + TIMEOUT + GRACE);
    CHECK_EQ_INT(candidateOf(g)->errors, 1);
    CHECK_EQ_INT(answer(g, t + 2 * TIMEOUT - 1, passed.seq, 1), 1);
    CHECK_EQ_INT(candidateOf(g)->samples, 3);
    CHECK_EQ_INT(candidateOf(g)->errors, 0);
    CHECK_EQ_INT(g->stats.missing, 0);
    gateFree(g);

    /* A request that was only wanted for training is simply forgotten. */
    g = classGate(2, 12);
    CHECK(askText(g, t, U_PLAIN, &XA).seq != 0);
    CHECK(askText(g, t, U_SPOT, &XB).seq != 0);
    expectIdle(g, t + TIMEOUT - 1);
    expectDue(g, t + TIMEOUT);
    CHECK_EQ_INT(g->npending, 0);
    CHECK_EQ_INT(g->pending_bytes, 0);
    CHECK_EQ_INT(g->stats.dropped, 2);
    CHECK_EQ_INT(g->stats.missing, 0);
    CHECK_EQ_INT(g->missing_run, 0);
    CHECK_EQ_INT(g->trainer.updates, 0);
    gateFree(g);
}

/* The proof of two gates is the same, bit for bit. */
static void sameRegions(const gate *a, const gate *b) {
    const gateGeneration *ga[2], *gb[2];
    int i;

    ga[0] = a->candidate;
    ga[1] = a->serving;
    gb[0] = b->candidate;
    gb[1] = b->serving;
    for (i = 0; i < 2; i++) {
        CHECK((ga[i] == NULL) == (gb[i] == NULL));
        if (ga[i] == NULL || gb[i] == NULL) continue;
        CHECK_EQ_INT(ga[i]->gen, gb[i]->gen);
        CHECK_EQ_INT(ga[i]->samples, gb[i]->samples);
        CHECK_EQ_INT(ga[i]->errors, gb[i]->errors);
        CHECK(memcmp(ga[i]->policy, gb[i]->policy, sizeof(int32_t) * a->nrows) == 0);
        CHECK(memcmp(ga[i]->regions, gb[i]->regions, sizeof(gateRegion) * a->nrows * T) == 0);
    }
}

/* A missing answer is a wrong guess to the proof, exactly: a gate whose
 * teacher never answers some requests holds, bit for bit, the regions of a
 * gate whose teacher answers them all wrong. To the drift detector it is
 * not a disagreement: a lost spot check is counted apart, in the statistic
 * that says "cannot be checked", and never in the one that says "worse". */
TEST(missing_is_an_error_for_the_proof_only) {
    gate *wrong = scoreGate(), *lost = scoreGate(), *g[2];
    gatePredictResult r[2][40];
    int64_t now = T0;
    uint32_t i;
    int k;

    g[0] = wrong;
    g[1] = lost;
    /* 40 requests; number 5 and 17 are answered wrong or not at all. */
    for (k = 0; k < 2; k++) {
        for (i = 0; i < 40; i++) r[k][i] = ask(g[k], now, U_PLAIN, 0.9);
        for (i = 0; i < 40; i++) {
            if (i == 5 || i == 17) {
                if (k == 0) CHECK_EQ_INT(answer(g[k], now, r[k][i].seq, 0), 1);
            } else {
                CHECK_EQ_INT(answer(g[k], now, r[k][i].seq, 1), 1);
            }
        }
    }
    now += TIMEOUT;
    for (k = 0; k < 2; k++) {
        gateMaintain(g[k], now);
        inv(g[k]);
    }
    sameRegions(wrong, lost);
    CHECK_EQ_INT(candidateOf(lost)->samples, 40);
    CHECK_EQ_INT(candidateOf(lost)->errors, 2);
    CHECK_EQ_INT(lost->stats.missing, 2);
    CHECK_EQ_INT(wrong->stats.missing, 0);
    /* Lost answers, not the beginning of an outage: 38 answers arrived
     * after the two requests were asked. */
    CHECK_EQ_INT(lost->missing_run, 0);
    CHECK_EQ_INT(wrong->missing_run, 0);
    CHECK_EQ_INT(lost->answer_mark, 41);
    CHECK_EQ_INT(wrong->answer_mark, 41);

    /* With two wrong answers the proof takes 57 correct ones: 19 more. */
    for (k = 0; k < 2; k++) {
        for (i = 0; i < 19; i++) {
            CHECK(gateState(g[k]) == GATE_STATE_CALIBRATING);
            feed(g[k], now, U_PLAIN, 0.9, 1);
        }
        CHECK(gateState(g[k]) == GATE_STATE_ACTIVE);
    }
    sameRegions(wrong, lost);
    CHECK_EQ_INT(lost->missing_run, 0); /* answers arrive */

    /* Six spot checks of answers, two of them wrong or lost. */
    for (k = 0; k < 2; k++) {
        for (i = 0; i < 6; i++) {
            r[k][i] = ask(g[k], now, U_SPOT, 0.9);
            CHECK(r[k][i].action == GATE_ACTION_AUDIT);
        }
        for (i = 0; i < 6; i++) {
            if (i == 1 || i == 4) {
                if (k == 0) CHECK_EQ_INT(answer(g[k], now, r[k][i].seq, 0), 1);
            } else {
                CHECK_EQ_INT(answer(g[k], now, r[k][i].seq, 1), 1);
            }
        }
    }
    now += TIMEOUT;
    for (k = 0; k < 2; k++) {
        gateMaintain(g[k], now);
        inv(g[k]);
        CHECK(g[k]->serving != NULL);
    }
    /* The same proof... */
    sameRegions(wrong, lost);
    CHECK_EQ_INT(servingOf(lost)->samples, 59 + 6);
    CHECK_EQ_INT(servingOf(lost)->errors, 4);
    CHECK_EQ_INT(lost->stats.missing, 4);
    /* ...but four answered spot checks, all right, where 'wrong' has six,
     * two of them wrong. Both detectors numbered six spot checks. That of
     * 'wrong' holds two disagreements: stat(6, 2) = 0.914 in both of its
     * statistics. That of 'lost' holds none and two lost answers: it is at
     * stat(6, 0) = (0.75^6 + 0.5^6) / 2 = 0.0968 for "worse" and at 0.914
     * for "cannot be checked". */
    CHECK_EQ_INT(servingOf(wrong)->audits, 6);
    CHECK_EQ_INT(servingOf(wrong)->audit_errors, 2);
    expectDrift(&servingOf(wrong)->drift[0], 6, 2, 2, 0);
    CHECK_NEAR(gateDriftLogStat(&servingOf(wrong)->drift[0], &wrong->drift_bets[0], 0),
               log(0.9140625), 1e-12);
    CHECK_NEAR(gateDriftLogStat(&servingOf(wrong)->drift[0], &wrong->drift_bets[0], 1),
               log(0.9140625), 1e-12);
    CHECK_EQ_INT(servingOf(lost)->audits, 4);
    CHECK_EQ_INT(servingOf(lost)->audit_errors, 0);
    expectDrift(&servingOf(lost)->drift[0], 6, 0, 0, 2);
    CHECK_NEAR(gateDriftLogStat(&servingOf(lost)->drift[0], &lost->drift_bets[0], 0),
               log((0.177978515625 + 0.015625) / 2.0), 1e-12);
    CHECK_NEAR(gateDriftLogStat(&servingOf(lost)->drift[0], &lost->drift_bets[0], 1),
               log(0.9140625), 1e-12);
    CHECK(wrong->blind == 0 && lost->blind == 0);
    /* The two lost spot checks had seen answers arrive since they were
     * asked (those of the four others): lost answers, not a run. */
    CHECK_EQ_INT(lost->missing_run, 0);
    gateFree(wrong);
    gateFree(lost);
}

/* ----------------------------------- Blind -------------------------------
 * When 32 pieces of evidence in a row end without an answer, and no answer
 * of any request has arrived since the first of them was asked, the gate
 * cannot check itself: it drops both generations, keeps its model, and
 * takes no snapshot until an answer arrives. The gate remembers the request
 * number that was next when the last answer was used (answer_mark): a
 * request below it has seen an answer arrive, and if it ends unanswered it
 * is a lost answer, not part of an outage. */

/* 'n' requests of a score gate that must be recorded and that nobody
 * answers, and their feedback timeout. Returns the time after it. */
static int64_t loseAnswers(gate *g, int64_t now, uint32_t n, double u) {
    uint32_t i;

    for (i = 0; i < n; i++) CHECK(ask(g, now, u, 0.9).seq != 0);
    gateMaintain(g, now + TIMEOUT);
    inv(g);
    return now + TIMEOUT;
}

TEST(blind_run) {
    gate *g = scoreGate();
    gatePredictResult head, r, late;
    int64_t t = T0;
    gateEval ev;
    uint32_t i;
    int answered;

    /* 31 answers missing in a row: one short. */
    t = loseAnswers(g, t, 31, U_PLAIN);
    CHECK_EQ_INT(g->stats.missing, 31);
    CHECK_EQ_INT(g->missing_run, 31);
    CHECK_EQ_INT(g->answer_mark, 0); /* no answer has ever been used */
    CHECK_EQ_INT(g->blind, 0);
    CHECK(g->candidate != NULL && candidateOf(g)->samples == 31 && candidateOf(g)->errors == 31);
    /* An answer that is not used does not end the run... */
    CHECK_EQ_INT(gateLearn(g, t, EPOCH, 999999, 1), 0);
    CHECK_EQ_INT(gateLearn(g, t, EPOCH + 1, 1, 1), 0);
    CHECK_EQ_INT(gateLearn(g, t, EPOCH, 1, 1), 0); /* a request whose time was up */
    CHECK_EQ_INT(g->missing_run, 31);
    CHECK_EQ_INT(g->answer_mark, 0);
    /* ...one that is used does, at once: this one still waits for its turn
     * behind an open request. Every request asked so far has now seen an
     * answer arrive. */
    head = ask(g, t, U_PLAIN, 0.9);
    r = ask(g, t, U_PLAIN, 0.9);
    CHECK(head.seq == 32 && r.seq == 33);
    CHECK_EQ_INT(answer(g, t, r.seq, 1), 1);
    CHECK_EQ_INT(flagsOf(g, r.seq), GATE_OBS_CAND | GATE_OBS_RESOLVED);
    CHECK_EQ_INT(g->missing_run, 0);
    CHECK_EQ_INT(g->answer_mark, 34);
    /* The open request and 30 more end without an answer. The open one was
     * asked before that answer arrived: a lost answer. It is a missing
     * answer and a wrong guess like the others, but the run is the 30. */
    t = loseAnswers(g, t, 30, U_PLAIN);
    CHECK_EQ_INT(g->stats.missing, 62);
    CHECK_EQ_INT(g->missing_run, 30);
    CHECK_EQ_INT(g->blind, 0);
    CHECK(g->candidate != NULL && candidateOf(g)->samples == 63 && candidateOf(g)->errors == 62);
    CHECK(gateFindObservation(g, head.seq) == NULL);
    /* The 31st, and the 32nd: the gate is blind. Nothing was serving, so
     * nothing was revoked; the generation number is not handed back. */
    t = loseAnswers(g, t, 1, U_PLAIN);
    CHECK_EQ_INT(g->stats.missing, 63);
    CHECK_EQ_INT(g->missing_run, 31);
    CHECK_EQ_INT(g->blind, 0);
    t = loseAnswers(g, t, 1, U_PLAIN);
    CHECK_EQ_INT(g->stats.missing, 64);
    CHECK_EQ_INT(g->blind, 1);
    CHECK_EQ_INT(g->missing_run, 0);
    CHECK_EQ_INT(g->answer_mark, 34);
    CHECK(g->candidate == NULL && g->serving == NULL);
    CHECK(gateState(g) == GATE_STATE_COLD);
    CHECK_EQ_INT(g->stats.revocations, 0);
    CHECK_EQ_INT(g->stats.retired, 0);
    CHECK_EQ_INT(g->generations, 1);
    gateFree(g);

    /* An answer says that the feedback path works, whichever request it is
     * for, whenever it comes and whatever it says. Here it is the late
     * answer of a request that proof has moved on without. 32 requests were
     * asked before it arrived and none of them gets an answer: with it they
     * are lost answers, and the gate goes on; without it they are a run of
     * 32, and the gate is blind. */
    for (answered = 0; answered < 2; answered++) {
        g = scoreGate();
        t = T0;
        late = ask(g, t, U_PLAIN, 0.9);
        r = ask(g, t, U_PLAIN, 0.9);
        CHECK_EQ_INT(answer(g, t, r.seq, 1), 1);
        CHECK_EQ_INT(g->answer_mark, 3);
        for (i = 0; i < 32; i++) CHECK(ask(g, t + 1, U_PLAIN, 0.9).seq != 0);
        expectDue(g, t + GRACE);
        CHECK_EQ_INT(flagsOf(g, late.seq), GATE_OBS_CAND | GATE_OBS_PROVISIONAL);
        if (answered) {
            CHECK_EQ_INT(answer(g, t + 900, late.seq, 0), 1);
            CHECK_EQ_INT(g->answer_mark, 35);
        }
        /* The time of the late request is up, if it is still there: it had
         * seen an answer arrive (the one proof moved on for). */
        gateMaintain(g, t + TIMEOUT);
        inv(g);
        CHECK_EQ_INT(g->stats.missing, answered ? 0 : 1);
        CHECK_EQ_INT(g->missing_run, 0);
        CHECK_EQ_INT(g->blind, 0);
        /* The time of the 32 is up. */
        gateMaintain(g, t + 1 + TIMEOUT);
        inv(g);
        CHECK_EQ_INT(g->stats.missing, answered ? 32 : 33);
        CHECK_EQ_INT(g->missing_run, 0);
        CHECK_EQ_INT(g->blind, !answered);
        CHECK_EQ_INT(g->candidate != NULL, answered);
        CHECK_EQ_INT(g->npending, 0);
        if (answered) CHECK(candidateOf(g)->samples == 34 && candidateOf(g)->errors == 33);
        gateFree(g);
    }

    /* The run is also ended by the answer of a request that is no proof at
     * all (here: a guess of generation 1 below its threshold, kept for
     * training). */
    g = twoGenerations(T0);
    t = T0;
    CHECK_EQ_INT(g->answer_mark, 40); /* 39 requests, each answered at once */
    for (i = 0; i < 3; i++) {
        ev = guesses(g, 0, 0.99, 0, 0.99);
        CHECK(askGuess(g, t, U_SPOT, &ev, &XA).action == GATE_ACTION_AUDIT);
    }
    expectDue(g, t + TIMEOUT);
    CHECK_EQ_INT(g->stats.missing, 3);
    CHECK_EQ_INT(g->missing_run, 3);
    ev = guesses(g, 0, 0.4, 0, 0.4);
    r = askGuess(g, t + TIMEOUT, U_PLAIN, &ev, &XA);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0);
    CHECK_EQ_INT(flagsOf(g, r.seq), 0);
    CHECK_EQ_INT(answer(g, t + TIMEOUT, r.seq, 0), 1);
    CHECK_EQ_INT(g->missing_run, 0);
    CHECK_EQ_INT(g->answer_mark, r.seq + 1);
    gateFree(g);
}

TEST(blind_score_gate) {
    int64_t t = T0;
    gate *g = scoreGate();
    gatePredictResult r, r2;
    const gateObs *o;
    uint32_t i;

    /* A gate that answers from the threshold 0.55: a wrong answer at 0.5
     * leaves the four thresholds below it unproven. */
    feed(g, t, U_PLAIN, 0.5, 0);
    for (i = 0; i < 35; i++) feed(g, t, U_PLAIN, 0.9, 1);
    CHECK(g->serving != NULL && servingOf(g)->policy[0] == 4);

    /* 32 requests under that threshold which the draw makes proof for the
     * thresholds below, and nobody answers them. They are not spot checks
     * of answers: the drift detector does not number them (what lost spot
     * checks do to it is in drift_lost_spot_checks). The gate goes on
     * answering until the time of the 32nd is up... */
    for (i = 0; i < 32; i++) {
        r = ask(g, t, U_SPOT, 0.5);
        CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0);
        CHECK_EQ_INT(flagsOf(g, r.seq), GATE_OBS_AUDIT | GATE_OBS_SERV);
        CHECK_EQ_INT(spotOf(g, r.seq), 0);
    }
    expectDrift(&servingOf(g)->drift[0], 0, 0, 0, 0);
    CHECK(ask(g, t + TIMEOUT - 1, U_PLAIN, 0.9).action == GATE_ACTION_ACCEPT);
    CHECK(gateState(g) == GATE_STATE_ACTIVE);
    expectDue(g, t + TIMEOUT);
    /* ...then it is blind: it stops, and that is a revocation. */
    CHECK_EQ_INT(g->blind, 1);
    CHECK_EQ_INT(g->missing_run, 0);
    CHECK(g->serving == NULL && g->candidate == NULL);
    CHECK(gateState(g) == GATE_STATE_COLD);
    CHECK_EQ_INT(g->stats.revocations, 1);
    CHECK_EQ_INT(g->stats.missing, 32);
    CHECK_EQ_INT(g->stats.dropped, 0);
    CHECK_EQ_INT(g->generations, 1);
    CHECK_EQ_INT(g->npending, 0);
    CHECK(gateCoverage(g) == 0.0 && gateRowThreshold(g, 0) < 0.0);
    /* A paused blind gate is PAUSED, as always. */
    gatePause(g, t + TIMEOUT);
    CHECK(gateState(g) == GATE_STATE_PAUSED);
    gateResume(g, t + TIMEOUT);
    CHECK(gateState(g) == GATE_STATE_COLD);

    /* No snapshot while blind, however long: a score gate with nothing
     * under test would otherwise start its next test at once. */
    expectIdle(g, t + 2 * TIMEOUT);
    CHECK_EQ_INT(gateMaintenanceDue(g, t + 1000 * TIMEOUT), 0);
    t += 2 * TIMEOUT;
    /* Every request is recorded, so that there is an id to answer. It is
     * proof for nothing, whatever the draw. */
    r = ask(g, t, U_PLAIN, 0.9);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0 && r.label == -1);
    CHECK_EQ_INT(flagsOf(g, r.seq), 0);
    r2 = ask(g, t, U_SPOT, 0.9);
    CHECK(r2.action == GATE_ACTION_ABSTAIN && r2.seq != 0);
    CHECK_EQ_INT(flagsOf(g, r2.seq), GATE_OBS_AUDIT);
    o = gateFindObservation(g, r.seq);
    CHECK(o != NULL && o->cand_gen == 0 && o->serv_gen == 0 && o->result == -1 && o->x.n == 0);
    CHECK_EQ_INT(g->nevidence, 0);
    CHECK_EQ_INT(gateUnanswered(g), 2);
    CHECK_EQ_INT(g->blind, 1);
    /* Unanswered, they are forgotten like requests kept for training. */
    expectIdle(g, t + TIMEOUT - 1);
    expectDue(g, t + TIMEOUT);
    CHECK_EQ_INT(g->stats.dropped, 2);
    CHECK_EQ_INT(g->stats.missing, 32);
    CHECK_EQ_INT(g->blind, 1);
    CHECK_EQ_INT(g->generations, 1);
    t += TIMEOUT;

    /* The first answer that is used ends it, and the next snapshot is
     * taken in that same call: generation 2, for which the answered
     * request is no proof. */
    r = ask(g, t, U_PLAIN, 0.9);
    CHECK_EQ_INT(gateLearn(g, t, EPOCH, r.seq + 1, 1), 0); /* no such request */
    CHECK_EQ_INT(g->blind, 1);
    CHECK_EQ_INT(gateMaintenanceDue(g, t + 7), 0);
    CHECK_EQ_INT(answer(g, t + 7, r.seq, 0), 1);
    CHECK_EQ_INT(g->blind, 0);
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 2 && g->serving == NULL);
    CHECK_EQ_INT(g->generations, 2);
    CHECK_EQ_INT(candidateOf(g)->frozen_ms, t + 7);
    CHECK_NEAR(candidateOf(g)->log_threshold, log(960.0), 1e-12);
    CHECK_EQ_INT(candidateOf(g)->samples, 0);
    CHECK_EQ_INT(gateMaintenanceDue(g, t + 7), 0);
    /* It proves itself again: 41 correct answers for generation 2. */
    for (i = 1; i <= 41; i++) {
        feed(g, t + 7, U_PLAIN, 0.9, 1);
        CHECK_EQ_INT(g->serving != NULL, i == 41);
    }
    CHECK_EQ_INT(g->stats.promotions, 2);
    CHECK_EQ_INT(g->stats.revocations, 1);
    gateFree(g);
}

/* A run of missing answers must not wipe the model of a classifier:
 * missing answers say nothing about it. */
TEST(blind_classifier) {
    int64_t t = T0;
    gate *g = trainedGate(12);
    gatePredictResult r;
    gateEval ev;
    uint64_t weights;
    uint32_t i;

    for (i = 0; i < 39; i++) feedText(g, t, U_PLAIN, &XA, 0);
    CHECK(g->serving != NULL && servingOf(g)->gen == 1 && servingOf(g)->policy[0] == 0);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 2);
    CHECK_EQ_INT(g->trainer.updates, 51);
    weights = mixModel(0, &g->trainer, 0);

    /* 31 requests for label1, which generation 1 does not answer yet, so
     * that every one of them is proof for it; nobody answers them. Wrong
     * answers would teach the model something; missing ones are wrong
     * guesses to the proof and nothing else. (They are not spot checks of
     * answers: see drift_lost_spot_checks for those.) */
    for (i = 0; i < 31; i++) {
        ev = guesses(g, 1, 0.8, 1, 0.8);
        r = askGuess(g, t, U_PLAIN, &ev, &XB);
        CHECK(r.action == GATE_ACTION_ABSTAIN);
        CHECK_EQ_INT(flagsOf(g, r.seq), GATE_OBS_SERV);
    }
    expectDue(g, t + TIMEOUT);
    CHECK(g->serving != NULL && g->candidate != NULL);
    CHECK_EQ_INT(g->stats.missing, 31);
    CHECK_EQ_INT(g->missing_run, 31);
    CHECK_EQ_INT(servingOf(g)->samples, 39 + 31);
    CHECK_EQ_INT(servingOf(g)->errors, 31);
    CHECK_EQ_INT(servingOf(g)->audits, 0);
    CHECK_EQ_INT(servingOf(g)->audit_errors, 0);
    expectDrift(&servingOf(g)->drift[0], 0, 0, 0, 0);
    expectDrift(&servingOf(g)->drift[1], 0, 0, 0, 0);
    CHECK(gateDriftLogStat(&servingOf(g)->drift[1], &g->drift_bets[1], 1) == 0.0);
    CHECK_EQ_INT(candidateOf(g)->samples, 0);
    CHECK_EQ_INT(g->stats.revocations, 0);
    CHECK_EQ_INT(g->trainer.updates, 51);
    CHECK(mixModel(0, &g->trainer, 0) == weights);
    t += TIMEOUT;
    CHECK(askText(g, t, U_PLAIN, &XA).action == GATE_ACTION_ACCEPT); /* still answering */

    /* The 32nd in a row: blind. Both generations go; the model stays, bit
     * for bit. */
    ev = guesses(g, 1, 0.8, 1, 0.8);
    CHECK(askGuess(g, t, U_PLAIN, &ev, &XB).seq != 0);
    expectDue(g, t + TIMEOUT);
    t += TIMEOUT;
    CHECK_EQ_INT(g->blind, 1);
    CHECK(g->serving == NULL && g->candidate == NULL);
    CHECK(gateState(g) == GATE_STATE_COLD);
    CHECK_EQ_INT(g->stats.revocations, 1);
    CHECK_EQ_INT(g->stats.missing, 32);
    CHECK_EQ_INT(g->generations, 2);
    CHECK_EQ_INT(g->trainer.updates, 51);
    CHECK(mixModel(0, &g->trainer, 0) == weights);
    CHECK_EQ_INT(g->snapshot_updates, 0);

    /* No snapshot while blind, although the model has learned more than
     * enough for one, and goes on learning from GADD. */
    expectIdle(g, t + 5);
    for (i = 0; i < 20; i++) train(g, t + 5, &XB, 1);
    CHECK(g->candidate == NULL && g->blind == 1);
    CHECK_EQ_INT(gateMaintenanceDue(g, t + 5), 0);
    CHECK_EQ_INT(g->generations, 2);
    /* Requests are recorded as in a gate that has no snapshot yet. */
    r = askText(g, t + 5, U_SPOT, &XA);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0);
    CHECK_EQ_INT(flagsOf(g, r.seq), GATE_OBS_AUDIT);
    /* Its answer ends the blindness and the snapshot is taken in the same
     * call, from the model as it is then: generation 3. */
    CHECK_EQ_INT(answer(g, t + 6, r.seq, 0), 1);
    CHECK_EQ_INT(g->blind, 0);
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 3);
    CHECK_EQ_INT(g->generations, 3);
    CHECK_EQ_INT(g->trainer.updates, 72);
    CHECK_EQ_INT(candidateOf(g)->model.updates, 72);
    CHECK_EQ_INT(g->snapshot_updates, 72);
    CHECK(mixModel(0, &candidateOf(g)->model, 0) == mixModel(0, &g->trainer, 0));
    CHECK_EQ_INT(candidateOf(g)->samples, 0);
    gateFree(g);
}

TEST(blind_records_up_to_the_training_limit) {
    int64_t t = T0;
    gate *g = scoreGate();
    gatePredictResult r;
    uint64_t first = 0, seq;
    uint32_t i;

    t = loseAnswers(g, t, 32, U_PLAIN);
    CHECK_EQ_INT(g->blind, 1);
    /* A blind gate records requests while fewer than 3072 wait for their
     * answer... */
    for (i = 0; i < GATE_TRAIN_PENDING; i++) {
        seq = quickAsk(g, t, (i & 1) ? U_SPOT : U_PLAIN, 0.9);
        if (seq == 0) break;
        if (i == 0) first = seq;
    }
    CHECK_EQ_INT(i, GATE_TRAIN_PENDING);
    inv(g);
    CHECK_EQ_INT(gateUnanswered(g), GATE_TRAIN_PENDING);
    CHECK(!gateFull(g));
    /* ...and no more: the next ones get no id. The gate is not full. */
    r = ask(g, t, U_PLAIN, 0.9);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq == 0);
    CHECK_EQ_INT(g->stats.full, 0);
    CHECK_EQ_INT(g->npending, GATE_TRAIN_PENDING);
    /* One answer is enough to see again. From then on requests are proof,
     * recorded up to 4096. */
    CHECK_EQ_INT(answer(g, t, first, 1), 1);
    CHECK(g->blind == 0 && g->candidate != NULL && candidateOf(g)->gen == 2);
    for (i = GATE_TRAIN_PENDING - 1; i < GATE_MAX_PENDING; i++)
        if (quickAsk(g, t, U_PLAIN, 0.9) == 0) break;
    CHECK_EQ_INT(i, GATE_MAX_PENDING);
    inv(g);
    CHECK(gateFull(g));
    CHECK_EQ_INT(g->nevidence, GATE_MAX_PENDING - GATE_TRAIN_PENDING + 1);
    gateFree(g);

    /* If none of the 3072 is answered, they are forgotten when their time
     * is up, and the gate hands out ids again: a blind gate can always be
     * answered within one feedback timeout. */
    g = scoreGate();
    t = loseAnswers(g, T0, 32, U_PLAIN);
    for (i = 0; i < GATE_TRAIN_PENDING; i++)
        if (quickAsk(g, t, U_PLAIN, 0.9) == 0) break;
    CHECK_EQ_INT(i, GATE_TRAIN_PENDING);
    CHECK(quickAsk(g, t + TIMEOUT - 1, U_PLAIN, 0.9) == 0);
    r = ask(g, t + TIMEOUT, U_PLAIN, 0.9);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0);
    CHECK_EQ_INT(g->npending, 1);
    CHECK_EQ_INT(g->stats.dropped, GATE_TRAIN_PENDING);
    CHECK_EQ_INT(g->stats.missing, 32);
    CHECK_EQ_INT(g->blind, 1);
    CHECK_EQ_INT(answer(g, t + TIMEOUT, r.seq, 1), 1);
    CHECK(g->blind == 0 && g->candidate != NULL);
    gateFree(g);
}

/* An outage of 5000 requests, one per millisecond, on a gate that answers:
 * no answer arrives. Every fourth request is a spot check. */
TEST(blind_outage_uses_no_generation) {
    int64_t t = T0;
    gate *g = activeScoreGate(t);
    gatePredictResult r;
    uint64_t last = 0;
    uint32_t i;

    memset(&r, 0, sizeof(r));
    for (i = 0; i < 5000; i++) {
        r = ask(g, t + i, (i % 4 == 0) ? U_SPOT : U_PLAIN, 0.9);
        last = r.seq;
        /* The spot check asked at millisecond 4k is a missing answer at
         * 4k + 1000. The 32nd is the one of millisecond 124: from request
         * 1124 on the gate is blind. Until then it answered. */
        CHECK_EQ_INT(g->blind, i >= 1124);
        CHECK_EQ_INT(r.action == GATE_ACTION_ACCEPT, i < 1124 && i % 4 != 0);
        CHECK_EQ_INT(r.action == GATE_ACTION_AUDIT, i < 1124 && i % 4 == 0);
        CHECK_EQ_INT(r.seq != 0, i >= 1124 || i % 4 == 0);
        /* No snapshot is taken, no number is used: not before the gate
         * went blind (a missing answer is no drift) and not after. */
        CHECK_EQ_INT(g->generations, 1);
        CHECK(g->candidate == NULL);
    }
    CHECK(gateState(g) == GATE_STATE_COLD);
    CHECK_EQ_INT(g->stats.revocations, 1);
    CHECK_EQ_INT(g->stats.promotions, 1);
    CHECK_EQ_INT(g->stats.missing, 32);
    /* 1124 requests before it went blind: 281 spot checks, 843 answers. */
    CHECK_EQ_INT(g->stats.requests, 35 + 5000);
    CHECK_EQ_INT(g->stats.accepted, 843);
    CHECK_EQ_INT(g->stats.audited, 281);
    CHECK_EQ_INT(g->stats.abstained, 35 + 3876);
    CHECK_EQ_INT(g->stats.observations, 35 + 281 + 3876);
    /* Forgotten: the 249 spot checks that were still open when the gate
     * went blind, and the requests 1124..3999, whose time was up. */
    CHECK_EQ_INT(g->stats.dropped, 249 + 2876);
    CHECK_EQ_INT(g->npending, 1000);
    CHECK_EQ_INT(g->stats.full, 0);
    /* The first answer after the outage: one snapshot, one number. */
    CHECK_EQ_INT(answer(g, t + 4999, last, 1), 1);
    CHECK_EQ_INT(g->blind, 0);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 2);
    CHECK_EQ_INT(g->generations, 2);
    gateFree(g);
}

TEST(blind_cleared_by_reset_and_recover) {
    int64_t t;
    uint32_t i;
    int reset;
    gate *g;

    for (reset = 0; reset < 2; reset++) {
        /* A blind gate sees again, and takes its next snapshot at once. */
        g = scoreGate();
        t = loseAnswers(g, T0, 32, U_PLAIN);
        CHECK(g->blind == 1 && g->candidate == NULL);
        if (reset) gateReset(g, t, EPOCH2); else gateRecover(g, t, EPOCH2);
        inv(g);
        CHECK_EQ_INT(g->blind, 0);
        CHECK_EQ_INT(g->missing_run, 0);
        CHECK(g->candidate != NULL && candidateOf(g)->gen == 2);
        CHECK(gateState(g) == GATE_STATE_CALIBRATING);
        gateFree(g);

        /* A run that has not come to 32 starts from nothing again: 31 more
         * missing answers do not make the gate blind. */
        g = scoreGate();
        t = loseAnswers(g, T0, 31, U_PLAIN);
        CHECK(g->blind == 0 && g->missing_run == 31);
        if (reset) gateReset(g, t, EPOCH2); else gateRecover(g, t, EPOCH2);
        inv(g);
        CHECK_EQ_INT(g->missing_run, 0);
        CHECK_EQ_INT(g->blind, 0);
        t = loseAnswers(g, t, 31, U_PLAIN);
        CHECK_EQ_INT(g->blind, 0);
        CHECK_EQ_INT(g->missing_run, 31);
        CHECK(g->candidate != NULL && candidateOf(g)->gen == 2);
        gateFree(g);
    }
    /* A blind classifier that is recovered freezes its model at once. */
    g = trainedGate(12);
    for (i = 0; i < 32; i++) CHECK(askText(g, T0, U_PLAIN, &XA).seq != 0);
    gateMaintain(g, T0 + TIMEOUT);
    inv(g);
    CHECK(g->blind == 1 && g->candidate == NULL && g->trainer.updates == 12);
    gateRecover(g, T0 + TIMEOUT, EPOCH2);
    inv(g);
    CHECK(g->blind == 0 && g->candidate != NULL && candidateOf(g)->gen == 2);
    gateFree(g);
}

/* A drift alarm starts the gate over, and the run of missing answers with
 * it. */
TEST(drift_alarm_clears_the_run) {
    int64_t t = T0;
    gate *g = activeScoreGate(t);
    gatePredictResult r;
    uint32_t i;

    /* Five spot checks that nobody answers: the gate still serves, with a
     * run of five. To the drift detector they are lost answers: for
     * "cannot be checked" the block is at stat(5, 5) = (32 + 243) / 2. */
    t = loseAnswers(g, t, 5, U_SPOT);
    CHECK(g->serving != NULL && g->missing_run == 5 && g->stats.missing == 5);
    CHECK_EQ_INT(servingOf(g)->audits, 0);
    expectDrift(&servingOf(g)->drift[0], 5, 0, 0, 5);
    CHECK_NEAR(gateDriftLogStat(&servingOf(g)->drift[0], &g->drift_bets[0], 1), log(137.5), 1e-12);
    /* Wrong spot checks until the alarm. For "worse" the five lost ones
     * count as right: the tenth disagreement (stat(15, 10) = 1044.1; nine
     * give 368.3). "Cannot be checked" is far: stat(15, 15) = 7.2e6. (An
     * answer that is used ends the run by itself.) */
    CHECK_NEAR(exp(driftStat(ALPHA, 15, 10)), 1044.140625, 1e-9);
    CHECK_NEAR(exp(driftStat(ALPHA, 14, 9)), 368.296875, 1e-9);
    for (i = 1; i <= 10; i++) {
        r = ask(g, t, U_SPOT, 0.9);
        CHECK_EQ_INT(answer(g, t, r.seq, 0), 1);
        CHECK_EQ_INT(g->missing_run, 0);
        CHECK_EQ_INT(g->serving != NULL, i < 10);
    }
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 2);
    CHECK_EQ_INT(g->stats.revocations, 1);
    CHECK_EQ_INT(g->blind, 0);
    /* 31 missing answers of generation 2 are a run of 31. */
    t = loseAnswers(g, t, 31, U_PLAIN);
    CHECK_EQ_INT(g->blind, 0);
    CHECK_EQ_INT(g->missing_run, 31);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 2 && candidateOf(g)->samples == 31);
    gateFree(g);
}

/* The feedback timeouts of 33 requests none of which was answered. The
 * 32nd makes the gate blind and drops the generation; the 33rd is then
 * proof for nothing: it is forgotten (dropped), it is not a missing answer,
 * and no new run of missing answers starts with it. 32 missing answers,
 * never 33: whether the last timeout is seen by the call that sees the
 * others or by a later one, and whichever function makes those calls. */
TEST(blind_then_queue_head) {
    enum { MAINTAIN, PREDICT, LEARN, PAUSE, TRAIN, HOWS };
    int64_t t = T0;
    int classifier, calls, how, step;

    for (classifier = 0; classifier < 2; classifier++) {
        for (calls = 1; calls <= 2; calls++) {
            for (how = 0; how < HOWS; how++) {
                gatePredictResult r;
                uint32_t i;
                gate *g;

                if (how == TRAIN && !classifier) continue;
                g = classifier ? trainedGate(12) : scoreGate();
                for (i = 0; i < 32; i++) {
                    r = classifier ? askText(g, t, U_PLAIN, &XA) : ask(g, t, U_PLAIN, 0.9);
                    CHECK(r.seq != 0);
                }
                /* The 33rd: a millisecond younger when its time is to be
                 * up in a call of its own. */
                r = classifier ? askText(g, t + calls - 1, U_PLAIN, &XA)
                               : ask(g, t + calls - 1, U_PLAIN, 0.9);
                CHECK_EQ_INT(flagsOf(g, r.seq), GATE_OBS_CAND);
                CHECK_EQ_INT(g->npending, 33);
                CHECK_EQ_INT(g->nevidence, 33);
                for (step = 0; step < calls; step++) {
                    int64_t now = t + TIMEOUT + step;

                    switch (how) {
                    case MAINTAIN: gateMaintain(g, now); break;
                    case PREDICT: /* recorded: the gate is blind by then */
                        if (classifier) askText(g, now, U_PLAIN, &XA); else ask(g, now, U_PLAIN, 0.9);
                        break;
                    case LEARN: CHECK_EQ_INT(gateLearn(g, now, EPOCH, 1, 0), 0); break;
                    case PAUSE:
                        gatePause(g, now);
                        gateResume(g, now);
                        break;
                    default: train(g, now, &XB, 1); break;
                    }
                    inv(g);
                    CHECK_EQ_INT(g->blind, 1);
                    CHECK(g->candidate == NULL && g->serving == NULL);
                    CHECK_EQ_INT(g->stats.missing, 32);
                    CHECK_EQ_INT(g->missing_run, 0);
                    /* A classifier keeps the 33rd for its label until its
                     * own time is up. */
                    CHECK_EQ_INT(g->stats.dropped, !(classifier && calls == 2 && step == 0));
                }
                CHECK_EQ_INT(g->stats.dropped, 1);
                CHECK_EQ_INT(g->npending, how == PREDICT ? calls : 0);
                CHECK_EQ_INT(g->nevidence, 0);
                CHECK_EQ_INT(g->stats.revocations, 0);
                CHECK_EQ_INT(g->generations, 1);
                CHECK_EQ_INT(answer(g, t + TIMEOUT + 2, r.seq, classifier ? 0 : 1), 0);
                gateFree(g);
            }
        }
    }

    /* The same when it is the drift detector that stops the gate, at the
     * timeout of a spot check that proof had moved on without. 24 spot
     * checks: 22 without an answer, one answered at once (which is why
     * proof moves on without the 22), one more without an answer, which
     * stays at the head of the queue. The 22nd lost answer takes the block
     * to stat(24, 22) = 3.9e9 (21 give 6.5e8): the gate cannot be checked
     * any more. 22 missing answers, never 23, and none of them part of a
     * run: each had seen an answer arrive. */
    CHECK(driftStat(ALPHA, 24, 22) >= log(1e9) && driftStat(ALPHA, 24, 21) < log(1e9));
    for (calls = 1; calls <= 2; calls++) {
        gate *g = activeScoreGate(t);
        gatePredictResult a, head;
        uint32_t i;

        for (i = 0; i < 22; i++) CHECK(ask(g, t, U_SPOT, 0.9).action == GATE_ACTION_AUDIT);
        a = ask(g, t, U_SPOT, 0.9);
        CHECK_EQ_INT(answer(g, t, a.seq, 1), 1);
        head = ask(g, t + calls - 1, U_SPOT, 0.9);
        CHECK_EQ_INT(spotOf(g, head.seq), 24);
        expectDue(g, t + GRACE);
        CHECK_EQ_INT(g->npending, 23);
        CHECK_EQ_INT(g->nevidence, 1);
        CHECK(g->ehead != NULL && g->ehead->seq == head.seq);
        CHECK_EQ_INT(servingOf(g)->errors, 22);
        CHECK_EQ_INT(servingOf(g)->audits, 1);
        expectDrift(&servingOf(g)->drift[0], 24, 0, 0, 0);
        gateMaintain(g, t + TIMEOUT);
        inv(g);
        CHECK_EQ_INT(g->blind, 1);
        if (calls == 2) {
            CHECK_EQ_INT(g->stats.missing, 22);
            gateMaintain(g, t + TIMEOUT + 1);
            inv(g);
        }
        CHECK_EQ_INT(g->blind, 1);
        CHECK(g->serving == NULL && g->candidate == NULL);
        CHECK_EQ_INT(g->stats.revocations, 1);
        CHECK_EQ_INT(g->stats.missing, 22);
        CHECK_EQ_INT(g->stats.dropped, 1);
        CHECK_EQ_INT(g->missing_run, 0);
        CHECK_EQ_INT(g->npending, 0);
        CHECK_EQ_INT(g->generations, 1);
        CHECK_EQ_INT(answer(g, t + TIMEOUT + 1, head.seq, 1), 0);
        gateFree(g);
    }
}

/* ------------------- Every threshold is a test of its own ---------------- */

#define B(t) (1u << (t))

/* The policy the gate derives for a row whose proven regions are 'mask'. */
static int32_t policyOf(const gate *g, gateGeneration *gen, uint32_t row, uint32_t mask) {
    uint32_t t;

    for (t = 0; t < T; t++) gen->regions[row * T + t].certified_at = (mask >> t) & 1;
    gateGenerationRefreshPolicy(g, gen, row);
    return gen->policy[row];
}

/* The definition: regions are nested, so a row is answered from its lowest
 * proven threshold, whatever is or is not proven above it. */
static int32_t policyByDefinition(uint32_t mask) {
    int32_t t;

    for (t = 0; t < (int32_t)T; t++)
        if ((mask >> t) & 1) return t;
    return -1;
}

TEST(policy_is_lowest_proven) {
    gateConfig cfg;
    gateGeneration *gen;
    uint64_t rng = 99;
    uint32_t i, r;
    gate *g;
    int kind;

    for (kind = 0; kind < 2; kind++) {
        uint32_t rows = kind ? 1 : 3;

        if (kind == 0) {
            classConfig(&cfg, 3, 10);
            cfg.class_defer[2] = 1;
            g = create(&cfg);
        } else {
            g = scoreGate();
        }
        gen = gateGenerationAllocate(g);
        if (gen == NULL) die("out of memory");

        /* An empty generation: no proof, no policy, nothing frozen. */
        CHECK(gen->gen == 0 && gen->samples == 0 && gen->errors == 0 && gen->missed == 0 &&
              gen->audits == 0 && gen->audit_errors == 0 && gen->budget == 0 &&
              gen->frozen_ms == 0 && gen->promoted_ms == 0 && gen->model.w == NULL);
        CHECK(gen->paired != NULL);
        for (i = 0; i < rows * T; i++) {
            const gateRegion *reg = &gen->regions[i];

            CHECK(reg->n == 0 && reg->errors == 0 && reg->certified_at == 0 &&
                  gateRegionLogWealth(reg) == 0.0);
            CHECK_EQ_INT(gen->paired[i], 0);
        }
        for (r = 0; r < rows; r++) {
            CHECK_EQ_INT(gen->policy[r], -1);
            expectDrift(&gen->drift[r], 0, 0, 0, 0);
            CHECK(gateDriftLogStat(&gen->drift[r], &g->drift_bets[r], 0) == 0.0);
            CHECK(gateDriftLogStat(&gen->drift[r], &g->drift_bets[r], 1) == 0.0);
        }

        CHECK_EQ_INT(policyOf(g, gen, 0, 0), -1);
        CHECK_EQ_INT(policyOf(g, gen, 0, B(31)), 31);
        CHECK_EQ_INT(policyOf(g, gen, 0, B(30)), 30);         /* nothing above it is needed */
        CHECK_EQ_INT(policyOf(g, gen, 0, B(31) | B(29)), 29); /* nor the one between */
        CHECK_EQ_INT(policyOf(g, gen, 0, B(22)), 22);
        CHECK_EQ_INT(policyOf(g, gen, 0, B(8) | B(7)), 7);
        CHECK_EQ_INT(policyOf(g, gen, 0, B(20) | B(5)), 5);
        CHECK_EQ_INT(policyOf(g, gen, 0, B(7) | B(6) | B(4)), 4);
        CHECK_EQ_INT(policyOf(g, gen, 0, B(0)), 0);
        CHECK_EQ_INT(policyOf(g, gen, 0, 0xfffffffeu), 1);
        CHECK_EQ_INT(policyOf(g, gen, 0, 0xffffffffu), 0);
        CHECK_EQ_INT(policyOf(g, gen, 0, 0x80000000u), 31);
        if (kind == 0) {
            /* One row at a time. */
            CHECK_EQ_INT(policyOf(g, gen, 1, B(15)), 15);
            CHECK_EQ_INT(gen->policy[0], 31);
            CHECK_EQ_INT(policyOf(g, gen, 0, 0), -1);
            CHECK_EQ_INT(gen->policy[1], 15);
        }
        for (i = 0; i < 20000; i++) {
            uint32_t mask = (uint32_t)gate_test_rand64(&rng);

            /* Sparse and dense patterns alike. */
            if (i % 3 == 1) mask &= (uint32_t)gate_test_rand64(&rng);
            if (i % 3 == 2) mask |= (uint32_t)gate_test_rand64(&rng);
            if (policyOf(g, gen, 0, mask) != policyByDefinition(mask)) break;
        }
        CHECK_EQ_INT(i, 20000);
        gateGenerationRelease(gen);
        gateFree(g);
    }
}

/* A live gate: thresholds are proven one by one, by the answers at or above
 * them, and the gate answers from the lowest proven one. */
TEST(every_threshold_on_its_own) {
    gate *g = trainedGate(500);
    gatePredictResult r;
    int64_t now = T0;
    gateEval ev;
    uint32_t i, t;

    /* A snapshot whose scores never get past 0.87: the thresholds 0..6.
     * Each of them is proven by its 39 correct answers; no threshold above
     * them has seen anything, and none is needed. */
    CHECK_EQ_INT(regionsOf(g, 0.87), 7);
    for (i = 1; i <= 39; i++) {
        ev = guesses(g, -1, 0.0, 0, 0.87);
        r = askGuess(g, now, U_PLAIN, &ev, &XA);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
        CHECK_EQ_INT(g->serving != NULL, i == 39);
    }
    for (t = 0; t < T; t++) CHECK_EQ_INT(servingOf(g)->regions[t].certified_at, t < 7 ? 39 : 0);
    CHECK_EQ_INT(servingOf(g)->policy[0], 0);
    CHECK_EQ_INT(servingOf(g)->policy[1], -1);
    CHECK(gateRowThreshold(g, 0) == 0.5);
    ev = guesses(g, 0, 0.5, -1, 0.0);
    r = askGuess(g, now, U_PLAIN, &ev, &XA);
    CHECK(r.action == GATE_ACTION_ACCEPT && r.label == 0);
    ev = guesses(g, 0, 0.9999, -1, 0.0); /* above every score it was proven on */
    r = askGuess(g, now, U_PLAIN, &ev, &XA);
    CHECK(r.action == GATE_ACTION_ACCEPT && r.label == 0);
    gateFree(g);

    /* Thresholds in the middle. Three wrong guesses at 0.6 (thresholds 0
     * and 1), then correct ones at 0.9 (thresholds 0..8): 2..8 are proven
     * by the 39th, 0 and 1 need 72. */
    g = trainedGate(500);
    CHECK_EQ_INT(regionsOf(g, 0.6), 2);
    CHECK_EQ_INT(regionsOf(g, 0.9), 9);
    for (i = 0; i < 3; i++) {
        ev = guesses(g, -1, 0.0, 0, 0.6);
        r = askGuess(g, now, U_PLAIN, &ev, &XA);
        CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
    }
    for (i = 1; i <= 39; i++) {
        ev = guesses(g, -1, 0.0, 0, 0.9);
        r = askGuess(g, now, U_PLAIN, &ev, &XA);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
        CHECK_EQ_INT(g->serving != NULL, i == 39);
    }
    for (t = 0; t < T; t++)
        CHECK_EQ_INT(servingOf(g)->regions[t].certified_at, t >= 2 && t < 9 ? 39 : 0);
    CHECK_EQ_INT(servingOf(g)->policy[0], 2);
    CHECK(gateRowThreshold(g, 0) == g->grid[2]);
    CHECK_NEAR(g->grid[2], 0.6652, 0.0001);
    /* Answered from there up, also above the highest proven threshold
     * (the regions are nested); not below. */
    ev = guesses(g, 0, 0.7, -1, 0.0);
    CHECK(askGuess(g, now, U_PLAIN, &ev, &XA).action == GATE_ACTION_ACCEPT);
    ev = guesses(g, 0, 0.99, -1, 0.0);
    CHECK(askGuess(g, now, U_PLAIN, &ev, &XA).action == GATE_ACTION_ACCEPT);
    ev = guesses(g, 0, 0.6, -1, 0.0);
    CHECK(askGuess(g, now, U_PLAIN, &ev, &XA).action == GATE_ACTION_ABSTAIN);
    /* The lower thresholds go on being tested on the spot checks: 33 more
     * correct answers make it 72, at 75 answers. */
    for (i = 40; i <= 72; i++) {
        ev = guesses(g, 0, 0.9, -1, 0.0);
        r = askGuess(g, now, U_SPOT, &ev, &XA);
        CHECK(r.action == GATE_ACTION_AUDIT);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
        CHECK_EQ_INT(servingOf(g)->policy[0], i < 72 ? 2 : 0);
    }
    CHECK_EQ_INT(servingOf(g)->regions[0].certified_at, 75);
    CHECK_EQ_INT(servingOf(g)->regions[1].certified_at, 75);
    CHECK_EQ_INT(servingOf(g)->regions[2].certified_at, 39);
    gateFree(g);

    /* The top alone. Two wrong guesses at 0.99 (thresholds 0..19), correct
     * ones at 1.0 (all 32): the thresholds 20..31 are proven first and the
     * gate answers from 0.991. */
    g = trainedGate(500);
    CHECK_EQ_INT(regionsOf(g, 0.99), 20);
    for (i = 0; i < 2; i++) {
        ev = guesses(g, -1, 0.0, 0, 0.99);
        r = askGuess(g, now, U_PLAIN, &ev, &XA);
        CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
    }
    for (i = 1; i <= 39; i++) {
        ev = guesses(g, -1, 0.0, 0, 1.0);
        r = askGuess(g, now, U_PLAIN, &ev, &XA);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
        CHECK_EQ_INT(g->serving != NULL, i == 39);
    }
    for (t = 0; t < T; t++) CHECK_EQ_INT(servingOf(g)->regions[t].certified_at, t >= 20 ? 39 : 0);
    CHECK_EQ_INT(servingOf(g)->policy[0], 20);
    CHECK(gateRowThreshold(g, 0) == g->grid[20]);
    CHECK_NEAR(g->grid[20], 0.9909, 0.0001);
    ev = guesses(g, 0, 0.995, -1, 0.0);
    CHECK(askGuess(g, now, U_PLAIN, &ev, &XA).action == GATE_ACTION_ACCEPT);
    ev = guesses(g, 0, 0.99, -1, 0.0);
    CHECK(askGuess(g, now, U_PLAIN, &ev, &XA).action == GATE_ACTION_ABSTAIN);
    /* 61 correct answers pay for two wrong ones: 22 more spot checks and
     * the gate answers everything. */
    for (i = 40; i <= 61; i++) {
        ev = guesses(g, 0, 1.0, -1, 0.0);
        r = askGuess(g, now, U_SPOT, &ev, &XA);
        CHECK(r.action == GATE_ACTION_AUDIT);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
        CHECK_EQ_INT(servingOf(g)->policy[0], i < 61 ? 20 : 0);
    }
    CHECK_EQ_INT(servingOf(g)->regions[0].certified_at, 63);
    CHECK_EQ_INT(servingOf(g)->regions[19].certified_at, 63);
    CHECK_EQ_INT(servingOf(g)->regions[20].certified_at, 39);
    gateFree(g);
}

/* A guess of a label that is never answered (ALWAYSDEFER) proves nothing:
 * the request is not proof for the generation that made the guess, and the
 * guess is counted in no region. (What it does count as, for a candidate,
 * is a request it could have been tested on, if the teacher's label is one
 * the gate may answer: see missed_requests.) */
TEST(guess_of_a_deferred_label_is_no_proof) {
    const uint32_t spot = GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_SERV;
    gatePredictResult r, late, held;
    gateConfig cfg;
    int64_t t = T0;
    gateEval ev;
    uint32_t i;
    gate *g;

    classConfig(&cfg, 3, 5);
    cfg.class_defer[2] = 1;
    g = create(&cfg);
    for (i = 0; i < 5; i++) train(g, t, &XA, 0);
    CHECK(g->candidate != NULL && g->serving == NULL);

    /* Nothing serves: every request is proof for the candidate, except
     * where it guesses label2. Those are kept for training only, whatever
     * the draw and whatever the answer. */
    ev = guesses(g, -1, 0.0, 2, 0.99);
    r = askGuess(g, t, U_PLAIN, &ev, &XC);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0);
    CHECK_EQ_INT(flagsOf(g, r.seq), 0);
    CHECK_EQ_INT(answer(g, t, r.seq, 2), 1);
    CHECK_EQ_INT(candidateOf(g)->missed, 0); /* the teacher said label2 too */
    r = askGuess(g, t, U_SPOT, &ev, &XC);
    CHECK_EQ_INT(flagsOf(g, r.seq), GATE_OBS_AUDIT);
    CHECK_EQ_INT(g->nevidence, 0);
    CHECK_EQ_INT(answer(g, t, r.seq, 0), 1);
    CHECK_EQ_INT(candidateOf(g)->samples, 0);
    CHECK_EQ_INT(candidateOf(g)->errors, 0);
    CHECK_EQ_INT(candidateOf(g)->missed, 1); /* label0: it could have been tested on this one */
    CHECK_EQ_INT(g->trainer.updates, 7);

    /* Generation 1 comes to answer label0 (39 correct guesses);
     * generation 2 is under test. */
    for (i = 0; i < 39; i++) {
        ev = guesses(g, -1, 0.0, 0, 0.99);
        r = askGuess(g, t, U_PLAIN, &ev, &XA);
        CHECK_EQ_INT(answer(g, t, r.seq, 0), 1);
    }
    CHECK(g->serving != NULL && servingOf(g)->gen == 1 && servingOf(g)->policy[0] == 0);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 2);
    CHECK_EQ_INT(servingOf(g)->samples, 39);

    /* A spot check: generation 1 says label0, generation 2 says label2.
     * Proof for generation 1 only. */
    ev = guesses(g, 0, 0.99, 2, 0.99);
    r = askGuess(g, t, U_SPOT, &ev, &XA);
    CHECK(r.action == GATE_ACTION_AUDIT);
    CHECK_EQ_INT(flagsOf(g, r.seq), spot);
    CHECK_EQ_INT(answer(g, t, r.seq, 0), 1);
    CHECK_EQ_INT(servingOf(g)->samples, 40);
    CHECK_EQ_INT(candidateOf(g)->samples, 0);
    expectPaired(g, candidateOf(g), NULL, 0); /* not one of its proof requests */
    CHECK_EQ_INT(candidateOf(g)->missed, 1);
    /* The other way round: generation 1 says label2, which it never
     * answers, so this is no spot check of an answer; generation 2 says
     * label0. Proof for generation 2 only. */
    ev = guesses(g, 2, 0.99, 0, 0.99);
    r = askGuess(g, t, U_SPOT, &ev, &XA);
    CHECK(r.action == GATE_ACTION_ABSTAIN);
    CHECK_EQ_INT(flagsOf(g, r.seq), GATE_OBS_AUDIT | GATE_OBS_CAND);
    CHECK_EQ_INT(spotOf(g, r.seq), 0);
    CHECK_EQ_INT(answer(g, t, r.seq, 0), 1);
    CHECK_EQ_INT(servingOf(g)->samples, 40);
    CHECK_EQ_INT(servingOf(g)->audits, 1);
    CHECK_EQ_INT(candidateOf(g)->samples, 1);
    /* Where generation 1 put that request is a label it never answers: it
     * is counted in no row of the paired counts. */
    expectPaired(g, candidateOf(g), NULL, 0);
    /* Without the spot check it is proof for nothing: kept for training. */
    r = askGuess(g, t, U_PLAIN, &ev, &XA);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0);
    CHECK_EQ_INT(flagsOf(g, r.seq), 0);
    /* Both say label2: nothing, spot check or not. */
    ev = guesses(g, 2, 0.99, 2, 0.99);
    r = askGuess(g, t, U_SPOT, &ev, &XC);
    CHECK_EQ_INT(flagsOf(g, r.seq), GATE_OBS_AUDIT);
    CHECK_EQ_INT(g->nevidence, 0);

    /* Late answers follow the same rule. Proof moves on without a spot
     * check for which generation 2 guessed label2: only generation 1 counts
     * the wrong guess, and takes it back. */
    ev = guesses(g, 0, 0.99, 2, 0.99);
    late = askGuess(g, t, U_SPOT, &ev, &XA);
    ev = guesses(g, 0, 0.99, 0, 0.99);
    held = askGuess(g, t, U_SPOT, &ev, &XA);
    CHECK_EQ_INT(answer(g, t, held.seq, 0), 1);
    gateMaintain(g, t + GRACE);
    inv(g);
    CHECK_EQ_INT(flagsOf(g, late.seq), spot | GATE_OBS_PROVISIONAL);
    CHECK_EQ_INT(servingOf(g)->samples, 42);
    CHECK_EQ_INT(servingOf(g)->errors, 1);
    CHECK_EQ_INT(candidateOf(g)->samples, 2);
    CHECK_EQ_INT(candidateOf(g)->errors, 0);
    CHECK_EQ_INT(answer(g, t + GRACE + 50, late.seq, 0), 1);
    CHECK_EQ_INT(servingOf(g)->samples, 42);
    CHECK_EQ_INT(servingOf(g)->errors, 0);
    CHECK_EQ_INT(candidateOf(g)->samples, 2);
    CHECK_EQ_INT(candidateOf(g)->errors, 0);
    /* No threshold of label2 has seen anything, in either generation. */
    for (i = 0; i < T; i++)
        CHECK_EQ_INT(candidateOf(g)->regions[2 * T + i].n + servingOf(g)->regions[2 * T + i].n, 0);
    CHECK(candidateOf(g)->gen == 2 && servingOf(g)->gen == 1);
    /* Of the two proof requests of generation 2, generation 1 put one at
     * label0, 0.99 (the other at label2). Two spot checks for which it
     * guessed label2 were answered label0. */
    {
        static const servGuess one = {0, 0.99, 1};

        expectPaired(g, candidateOf(g), &one, 1);
    }
    CHECK_EQ_INT(candidateOf(g)->missed, 2);
    gateFree(g);
}

/* ------------------------- The bar, per generation ----------------------- */

TEST(bar_per_generation) {
    gateConfig cfg;
    gate *g = scoreGate();
    int64_t now = T0;
    uint32_t i;

    /* Score gate: one row of 32 thresholds. A reset starts the next
     * generation; the budget of the earlier ones is gone for good. */
    CHECK_NEAR(candidateOf(g)->log_threshold, log(2.0 * 32.0 / 0.2), 1e-12);
    CHECK_NEAR(gateBudgetSpentNow(g), DELTA * 1.0 / 2.0, 1e-15);
    gateReset(g, now, EPOCH2);
    CHECK_EQ_INT(candidateOf(g)->gen, 2);
    CHECK_NEAR(candidateOf(g)->log_threshold, log(6.0 * 32.0 / 0.2), 1e-12);
    CHECK_NEAR(gateBudgetSpentNow(g), DELTA * 2.0 / 3.0, 1e-15);
    gateReset(g, now, EPOCH3);
    CHECK_EQ_INT(candidateOf(g)->gen, 3);
    CHECK_NEAR(candidateOf(g)->log_threshold, log(12.0 * 32.0 / 0.2), 1e-12);
    CHECK_NEAR(gateBudgetSpentNow(g), DELTA * 3.0 / 4.0, 1e-15);
    gateFree(g);

    baseConfig(&cfg);
    cfg.delta = 0.01;
    g = create(&cfg);
    CHECK_NEAR(candidateOf(g)->log_threshold, log(2.0 * 32.0 / 0.01), 1e-12);
    gateFree(g);

    /* Classifier: tested labels times 32 thresholds. A recovery keeps the
     * model, so each one freezes the next generation at once. The budget
     * of a candidate is, for each tested label, 10 times the proof a
     * flawless threshold needs: the correct answers in a row that take the
     * average of the four bets to the bar (file comment). */
    g = classGate(3, 3);
    CHECK_NEAR(gateBudgetSpentNow(g), 0.0, 0.0);
    for (i = 0; i < 3; i++) train(g, now, &XA, 0);
    CHECK_EQ_INT(candidateOf(g)->gen, 1);
    CHECK_NEAR(candidateOf(g)->log_threshold, log(2.0 * 96.0 / 0.2), 1e-12);
    CHECK_EQ_INT(candidateOf(g)->budget, 3 * 10 * 41); /* 41 to reach 960 */
    gateRecover(g, now, EPOCH2);
    CHECK_EQ_INT(candidateOf(g)->gen, 2);
    CHECK_NEAR(candidateOf(g)->log_threshold, log(6.0 * 96.0 / 0.2), 1e-12);
    CHECK_EQ_INT(candidateOf(g)->budget, 3 * 10 * 46); /* 46 to reach 2880 */
    gateRecover(g, now, EPOCH3);
    CHECK_EQ_INT(candidateOf(g)->gen, 3);
    CHECK_NEAR(candidateOf(g)->log_threshold, log(12.0 * 96.0 / 0.2), 1e-12);
    CHECK_EQ_INT(candidateOf(g)->budget, 3 * 10 * 50); /* 50 to reach 5760 */
    inv(g);
    gateFree(g);

    /* Two labels: 39 to reach 640, 44 to reach 1920, 48 to reach 3840. */
    g = classGate(2, 3);
    for (i = 0; i < 3; i++) train(g, now, &XA, 0);
    CHECK_EQ_INT(candidateOf(g)->budget, 780);
    gateRecover(g, now, EPOCH2);
    CHECK_EQ_INT(candidateOf(g)->budget, 880);
    gateRecover(g, now, EPOCH3);
    CHECK_EQ_INT(candidateOf(g)->budget, 960);
    gateFree(g);

    /* An ALWAYSDEFER label is not tested and takes no share. */
    classConfig(&cfg, 3, 3);
    cfg.class_defer[1] = 1;
    g = create(&cfg);
    for (i = 0; i < 3; i++) train(g, now, &XA, 0);
    CHECK_NEAR(candidateOf(g)->log_threshold, log(2.0 * 64.0 / 0.2), 1e-12);
    CHECK_EQ_INT(candidateOf(g)->budget, 2 * 10 * 39);
    gateFree(g);

    /* A stricter label decides: for an error rate of 0.1 it takes 82
     * correct answers to reach 640 (39 for 0.2), whichever label it is. */
    classConfig(&cfg, 3, 3);
    cfg.class_defer[0] = 1;
    cfg.class_alpha[2] = 0.1;
    g = create(&cfg);
    for (i = 0; i < 3; i++) train(g, now, &XA, 0);
    CHECK_EQ_INT(candidateOf(g)->budget, 2 * 10 * 82);
    gateFree(g);
    /* ... unless it is never answered. */
    classConfig(&cfg, 3, 3);
    cfg.class_defer[2] = 1;
    cfg.class_alpha[2] = 0.1;
    g = create(&cfg);
    for (i = 0; i < 3; i++) train(g, now, &XA, 0);
    CHECK_EQ_INT(candidateOf(g)->budget, 2 * 10 * 39);
    gateFree(g);
    /* With three tested labels the wealth to reach is 960: 87 correct
     * answers for an error rate of 0.1 (41 for 0.2), and it is the first,
     * the second or the third label that is the strict one. */
    CHECK_EQ_INT(needCorrect(0.1, log(960.0), 0), 87);
    for (i = 0; i < 3; i++) {
        uint32_t k;

        classConfig(&cfg, 3, 3);
        cfg.class_alpha[i] = 0.1;
        g = create(&cfg);
        for (k = 0; k < 3; k++) train(g, now, &XA, 0);
        CHECK_EQ_INT(candidateOf(g)->budget, 3 * 10 * 87);
        gateFree(g);
    }

    /* Many labels: each gets its share, so that a snapshot whose proof is
     * spread evenly over 64 labels is not dropped before any of them can
     * finish. 64 x 32 thresholds: the wealth to reach is 20480, which
     * takes 56 correct answers. */
    g = classGate(64, 3);
    for (i = 0; i < 3; i++) train(g, now, &XA, 0);
    CHECK_NEAR(candidateOf(g)->log_threshold, log(20480.0), 1e-12);
    CHECK_EQ_INT(candidateOf(g)->budget, 64 * 10 * 56);
    gateFree(g);
    /* One tested label of three: like a score gate's bar, one share. */
    classConfig(&cfg, 3, 3);
    cfg.class_defer[0] = cfg.class_defer[2] = 1;
    g = create(&cfg);
    for (i = 0; i < 3; i++) train(g, now, &XA, 0);
    CHECK_NEAR(candidateOf(g)->log_threshold, log(320.0), 1e-12);
    CHECK_EQ_INT(candidateOf(g)->budget, 1 * 10 * 35);
    gateFree(g);
}

/* -------------------------------- Replacing ------------------------------ */

TEST(replacing_needs_more_accepts) {
    /* What generation 1 guessed for the proof requests of generation 2, as
     * they come in below. */
    static const servGuess seen[] = {{0, 0.99, 44}, {0, 0.99, 1}, {0, 0.6, 1}};
    gate *g = classGate(2, 5);
    gatePredictResult r, late;
    int64_t now = T0;
    gateEval ev;
    uint32_t i;

    for (i = 0; i < 5; i++) train(g, now, &XA, 0);
    CHECK_EQ_INT(candidateOf(g)->budget, 2 * 10 * 39); /* 39 correct answers reach 640 */

    /* Generation 1 guesses label0. Ten wrong guesses at 0.6 put its
     * thresholds 0 and 1 out of reach; 39 correct ones at 0.99 prove the
     * thresholds 2..19, so it answers from threshold 2 (0.665). */
    CHECK_EQ_INT(regionsOf(g, 0.6), 2);
    CHECK_EQ_INT(regionsOf(g, 0.99), 20);
    for (i = 0; i < 10; i++) {
        ev = guesses(g, -1, 0.0, 0, 0.6);
        r = askGuess(g, now, U_PLAIN, &ev, &XA);
        CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
    }
    for (i = 1; i <= 39; i++) {
        ev = guesses(g, -1, 0.0, 0, 0.99);
        r = askGuess(g, now, U_PLAIN, &ev, &XA);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
        CHECK((g->serving != NULL) == (i == 39));
    }
    CHECK_EQ_INT(servingOf(g)->policy[0], 2);
    for (i = 0; i < T; i++)
        CHECK_EQ_INT(servingOf(g)->regions[i].certified_at, i >= 2 && i < 20 ? 39 : 0);
    /* 49 answers were learned since the first snapshot: the next one is
     * taken in the same breath. */
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 2);
    CHECK_NEAR(candidateOf(g)->log_threshold, log(1920.0), 1e-12);
    CHECK_EQ_INT(candidateOf(g)->budget, 2 * 10 * 44); /* 44 correct answers reach 1920 */
    expectPaired(g, candidateOf(g), NULL, 0);

    /* 44 spot checks that both generations would answer, all correct.
     * Generation 2 now has proven thresholds of its own (from 0.5), but it
     * accepted exactly the requests generation 1 accepted. */
    for (i = 1; i <= 44; i++) {
        ev = guesses(g, 0, 0.99, 0, 0.99);
        r = askGuess(g, now, U_SPOT, &ev, &XA);
        CHECK(r.action == GATE_ACTION_AUDIT);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
        CHECK_EQ_INT(candidateOf(g)->policy[0], i < 44 ? -1 : 0);
    }
    /* The paired counts: 44 at the thresholds 0..19 of label0 (0.99 is at
     * or above those), and generation 1 answers label0 from threshold 2. */
    expectPaired(g, candidateOf(g), seen, 1);
    CHECK_EQ_INT(candidateOf(g)->paired[0 * T + 2], 44);
    CHECK_EQ_INT(candidateOf(g)->regions[0].n, 44);
    CHECK(servingOf(g)->gen == 1 && candidateOf(g)->gen == 2);

    /* One that generation 1 would answer and generation 2 would not (it
     * guesses label1, where it has proven nothing): one behind. */
    ev = guesses(g, 0, 0.99, 1, 0.7);
    r = askGuess(g, now, U_SPOT, &ev, &XA);
    CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
    expectPaired(g, candidateOf(g), seen, 2);
    CHECK_EQ_INT(candidateOf(g)->paired[0 * T + 2], 45);
    CHECK(servingOf(g)->gen == 1);
    /* One that only generation 2 would answer (0.6 is under generation
     * 1's threshold): level. Level is not more. It is counted at the two
     * thresholds 0.6 reaches, where generation 1 does not answer. */
    ev = guesses(g, 0, 0.6, 0, 0.99);
    r = askGuess(g, now, U_SPOT, &ev, &XA);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0);
    CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
    CHECK_EQ_INT(candidateOf(g)->regions[0].n, 45);
    expectPaired(g, candidateOf(g), seen, 3);
    CHECK_EQ_INT(candidateOf(g)->paired[0 * T + 0], 46);
    CHECK_EQ_INT(candidateOf(g)->paired[0 * T + 1], 46);
    CHECK_EQ_INT(candidateOf(g)->paired[0 * T + 2], 45);
    CHECK(servingOf(g)->gen == 1 && servingOf(g)->policy[0] == 2);
    /* Another one, and a spot check that stays open: generation 1 guesses
     * label1 for it (unproven: not answered), generation 2 label0. */
    r = askGuess(g, now, U_SPOT, &ev, &XA);
    ev = guesses(g, 1, 0.8, 0, 0.99);
    late = askGuess(g, now, U_SPOT, &ev, &XA);
    CHECK_EQ_INT(flagsOf(g, late.seq), GATE_OBS_AUDIT | GATE_OBS_SERV | GATE_OBS_CAND);
    CHECK_NEAR(gateCoverage(g), 45.0 / 48.0, 1e-12); /* generation 1: all but the last three */
    /* Generation 2 takes over with this very answer. Its coverage is
     * measured from nothing again. */
    CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
    CHECK_EQ_INT(servingOf(g)->gen, 2);
    CHECK(g->stats.window == 0 && gateCoverage(g) == 0.0);
    CHECK_EQ_INT(servingOf(g)->samples, 47);
    /* The open spot check is still proof for generation 2, by what
     * generation 2 guessed. What generation 1 guessed went with it, and
     * generation 3 was not there to guess anything. */
    CHECK_EQ_INT(answer(g, now, late.seq, 0), 1);
    CHECK_EQ_INT(servingOf(g)->samples, 48);
    CHECK_EQ_INT(servingOf(g)->errors, 0);
    CHECK_EQ_INT(servingOf(g)->regions[0 * T].n, 47);
    CHECK_EQ_INT(servingOf(g)->regions[1 * T].n, 1);
    CHECK_EQ_INT(servingOf(g)->audits, 0);
    CHECK_EQ_INT(candidateOf(g)->samples, 0);
    CHECK_EQ_INT(servingOf(g)->policy[0], 0);
    /* What it was compared by is done with: every count is zero again,
     * and it has no budget to run out of. Its drift detectors start with
     * its own first spot check. */
    expectPaired(g, servingOf(g), NULL, 0);
    CHECK_EQ_INT(servingOf(g)->budget, 0);
    expectDrift(&servingOf(g)->drift[0], 0, 0, 0, 0);
    expectDrift(&servingOf(g)->drift[1], 0, 0, 0, 0);
    CHECK_EQ_INT(servingOf(g)->promoted_ms, now);
    CHECK_EQ_INT(g->stats.promotions, 2);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 3);
    CHECK_EQ_INT(candidateOf(g)->budget, 2 * 10 * 48); /* 48 correct answers reach 3840 */
    expectPaired(g, candidateOf(g), NULL, 0);
    gateFree(g);
}

TEST(candidate_budget) {
    gate *g = trainedGate(900);
    gatePredictResult r, late;
    int64_t now = T0;
    uint32_t i, group;

    /* The snapshot says label0 for XA and the teacher says label1 every
     * time. At 780 answers (two labels, 10 x 39 each: the proof a flawless
     * threshold needs) without a proven threshold it goes. */
    CHECK_EQ_INT(candidateOf(g)->budget, 780);
    for (i = 1; i < 780; i++) {
        r = askText(g, now, U_PLAIN, &XA);
        CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
    }
    CHECK(g->candidate != NULL && candidateOf(g)->samples == 779 && candidateOf(g)->errors == 779);
    CHECK_EQ_INT(candidateOf(g)->missed, 0);
    r = askText(g, now, U_PLAIN, &XA);
    late = askText(g, now, U_PLAIN, &XA); /* proof for it, answered after it is gone */
    CHECK_EQ_INT(flagsOf(g, late.seq), GATE_OBS_CAND);
    CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
    CHECK(g->candidate == NULL && g->serving == NULL);
    CHECK(gateState(g) == GATE_STATE_COLD);
    CHECK_EQ_INT(g->stats.retired, 1);
    CHECK_EQ_INT(g->generations, 1);
    /* What was proof for the dropped snapshot is still good for training. */
    CHECK_EQ_INT(flagsOf(g, late.seq), 0);
    CHECK_EQ_INT(g->nevidence, 0);
    CHECK_EQ_INT(g->npending, 1);
    CHECK_EQ_INT(answer(g, now, late.seq, 1), 1);
    CHECK_EQ_INT(g->npending, 0);

    /* The next snapshot comes 900 examples after the previous one: 781
     * answers were learned since, 119 to go. */
    CHECK_EQ_INT(g->trainer.updates, 900 + 781);
    for (i = 0; i < 118; i++) train(g, now, &XB, 1);
    CHECK(g->candidate == NULL);
    train(g, now, &XB, 1);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 2);
    CHECK_EQ_INT(candidateOf(g)->budget, 880); /* 10 x 44 each */
    gateFree(g);

    /* With a short step the examples are already there when the budget
     * runs out, and the next snapshot replaces the dropped one at once. */
    g = trainedGate(6);
    for (i = 1; i <= 780; i++) {
        r = askText(g, now, U_PLAIN, &XA);
        CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
        CHECK_EQ_INT(candidateOf(g)->gen, i < 780 ? 1 : 2);
    }
    CHECK_EQ_INT(g->stats.retired, 1);
    CHECK_EQ_INT(candidateOf(g)->samples, 0);
    CHECK_EQ_INT(candidateOf(g)->missed, 0);
    gateFree(g);

    /* Lost answers use the budget up like wrong ones. 24 rounds of one
     * wrong answer and 31 lost ones (32 lost ones in a row with no answer
     * in between would make the gate blind instead) are 768; a 25th round
     * of one wrong answer and eleven lost ones makes it 780. 779 leave the
     * snapshot in place; the 780th, a missing one, is its last. Only 25
     * examples were learned meanwhile, so no new snapshot follows. */
    g = trainedGate(900);
    for (group = 1; group <= 25; group++) {
        const uint32_t lose = group < 25 ? 31 : 11;

        r = askText(g, now, U_PLAIN, &XA);
        CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
        /* The very last request is asked a millisecond later. */
        for (i = 1; i <= lose; i++)
            CHECK(askText(g, now + (group == 25 && i == lose), U_PLAIN, &XA).seq != 0);
        now += TIMEOUT;
        gateMaintain(g, now);
        inv(g);
        CHECK_EQ_INT(g->blind, 0);
        if (group < 25) {
            CHECK_EQ_INT(candidateOf(g)->samples, 32 * group);
            CHECK_EQ_INT(g->stats.missing, 31 * group);
            CHECK_EQ_INT(g->missing_run, 31);
        }
    }
    CHECK(g->candidate != NULL && candidateOf(g)->samples == 779 && candidateOf(g)->errors == 779);
    CHECK_EQ_INT(g->stats.missing, 24 * 31 + 10);
    CHECK_EQ_INT(g->missing_run, 10);
    CHECK_EQ_INT(g->stats.retired, 0);
    gateMaintain(g, now + 1);
    inv(g);
    CHECK(g->candidate == NULL);
    CHECK_EQ_INT(g->stats.retired, 1);
    CHECK_EQ_INT(g->stats.missing, 24 * 31 + 11);
    CHECK_EQ_INT(g->blind, 0);
    CHECK_EQ_INT(g->trainer.updates, 925);
    CHECK(gateState(g) == GATE_STATE_COLD);
    gateFree(g);
}

TEST(score_candidate_has_no_budget) {
    gate *g = scoreGate();
    uint32_t i;

    CHECK_EQ_INT(candidateOf(g)->budget, 0);
    for (i = 0; i < 2000; i++) feed(g, T0, U_PLAIN, 1.0, 0);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 1);
    CHECK_EQ_INT(candidateOf(g)->samples, 2000);
    CHECK_EQ_INT(candidateOf(g)->missed, 0); /* a score gate has no label to miss */
    CHECK_EQ_INT(g->stats.retired, 0);
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    gateFree(g);
}

/* ---------------------------------- Drift -------------------------------- */

TEST(drift_score) {
    static const struct {
        const char *pattern; /* 'x' a wrong spot check, '.' a right one */
        uint32_t alarm;
    } cases[] = {{"xxxxxxx", 7}, {"xxxx...xxxxx", 12}, {"xxxxxx.xx", 9}, {"......xxxxxxxxxxx", 17}};
    int64_t now = T0;
    gate *g = activeScoreGate(now);
    gatePredictResult r, waiting;
    uint32_t i;
    size_t c;

    /* Wrong spot checks in a row: the gate answers until the alarm. Each
     * wrong answer is for the request before the newest, so the spot check
     * asked last never has its answer yet and counts as right: the eighth
     * disagreement (stat(9, 8) = 1736.25; seven of eight give 594.75). */
    CHECK_EQ_INT(alarmAfter(ALPHA, "oxxxxxxxx"), 9);
    waiting = ask(g, now, U_SPOT, 0.9);
    for (i = 1; i <= 8; i++) {
        r = waiting;
        waiting = ask(g, now, U_SPOT, 0.9);
        CHECK(waiting.action == GATE_ACTION_AUDIT);
        CHECK_EQ_INT(spotOf(g, r.seq), i);
        CHECK_EQ_INT(spotOf(g, waiting.seq), i + 1);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
        if (i < 8) {
            CHECK(gateState(g) == GATE_STATE_ACTIVE);
            CHECK_EQ_INT(servingOf(g)->audits, i);
            CHECK_EQ_INT(servingOf(g)->audit_errors, i);
            expectDrift(&servingOf(g)->drift[0], i + 1, i, i, 0);
            CHECK(ask(g, now, U_PLAIN, 0.9).action == GATE_ACTION_ACCEPT);
        }
    }
    /* Alarm: the generation is gone and, a score gate having nothing to
     * relearn, the next one is under test already. */
    CHECK(g->serving == NULL);
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 2);
    CHECK_EQ_INT(g->generations, 2);
    CHECK_EQ_INT(candidateOf(g)->samples, 0);
    CHECK_NEAR(candidateOf(g)->log_threshold, log(960.0), 1e-12);
    CHECK_EQ_INT(g->stats.revocations, 1);
    CHECK(gateCoverage(g) == 0.0);
    CHECK(ask(g, now, U_PLAIN, 0.9).action == GATE_ACTION_ABSTAIN);
    /* The spot check that was still open was proof for a generation that
     * no longer exists: its id is void. */
    CHECK(gateFindObservation(g, waiting.seq) == NULL);
    CHECK_EQ_INT(answer(g, now, waiting.seq, 1), 0);
    CHECK_EQ_INT(g->stats.dropped, 1);
    gateFree(g);

    /* Other histories, each on a fresh gate, every spot check answered at
     * once; the step at which the alarm is raised is derived by hand in
     * expected_numbers. Correct spot checks delay it, whether they come in
     * between or before: a block does not start over. */
    for (c = 0; c < COUNTOF(cases); c++) {
        uint32_t wrong = 0;

        g = activeScoreGate(now);
        CHECK_EQ_INT(alarmAfter(ALPHA, cases[c].pattern), cases[c].alarm);
        CHECK_EQ_INT(strlen(cases[c].pattern), cases[c].alarm);
        for (i = 1; i <= cases[c].alarm; i++) {
            r = ask(g, now, U_SPOT, 0.9);
            CHECK_EQ_INT(answer(g, now, r.seq, cases[c].pattern[i - 1] == '.'), 1);
            CHECK((g->serving != NULL) == (i < cases[c].alarm));
            wrong += cases[c].pattern[i - 1] == 'x';
            if (g->serving == NULL) continue;
            expectDrift(&servingOf(g)->drift[0], i, wrong, wrong, 0);
            CHECK_NEAR(gateDriftLogStat(&servingOf(g)->drift[0], &g->drift_bets[0], 0),
                       driftStat(ALPHA, i, wrong), 1e-12);
            CHECK(driftStat(ALPHA, i, wrong) < log(1000.0));
        }
        CHECK(driftStat(ALPHA, cases[c].alarm, wrong) >= log(1000.0));
        CHECK_EQ_INT(g->stats.revocations, 1);
        CHECK_EQ_INT(g->blind, 0);
        CHECK(g->candidate != NULL && candidateOf(g)->gen == 2);
        gateFree(g);
    }
    /* Only spot checks of answers count. Wrong proof from below the
     * threshold, however much, is not drift. */
    g = scoreGate();
    feed(g, now, U_PLAIN, 0.5, 0);
    for (i = 0; i < 35; i++) feed(g, now, U_PLAIN, 0.9, 1);
    CHECK_EQ_INT(servingOf(g)->policy[0], 4);
    for (i = 0; i < 50; i++) {
        r = ask(g, now, U_SPOT, 0.5);
        CHECK(r.action == GATE_ACTION_ABSTAIN);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
    }
    CHECK(g->serving != NULL && servingOf(g)->audits == 0);
    expectDrift(&servingOf(g)->drift[0], 0, 0, 0, 0);
    CHECK_EQ_INT(servingOf(g)->errors, 51);
    gateFree(g);
}

TEST(drift_classifier) {
    gate *g = trainedGate(12);
    gatePredictResult r;
    int64_t now = T0;
    uint32_t i, nonzero = 0;

    for (i = 0; i < 39; i++) feedText(g, now, U_PLAIN, &XA, 0);
    for (i = 0; i < 39; i++) feedText(g, now, U_PLAIN, &XB, 1);
    CHECK(gateState(g) == GATE_STATE_ACTIVE);
    CHECK(servingOf(g)->gen == 1 && servingOf(g)->policy[0] == 0 && servingOf(g)->policy[1] == 0);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 2);
    CHECK_EQ_INT(g->trainer.updates, 90);

    /* Each label is watched by its own detector: six wrong spot checks of
     * label1 answers are one short of an alarm, and stay one short however
     * label0 does. */
    for (i = 0; i < 6; i++) {
        r = askText(g, now, U_SPOT, &XB);
        CHECK(r.action == GATE_ACTION_AUDIT);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
    }
    CHECK(g->serving != NULL);
    expectDrift(&servingOf(g)->drift[1], 6, 6, 6, 0);
    expectDrift(&servingOf(g)->drift[0], 0, 0, 0, 0);
    CHECK_NEAR(gateDriftLogStat(&servingOf(g)->drift[1], &g->drift_bets[1], 0), log(396.5), 1e-12);
    /* The teacher starts calling XA label1. */
    for (i = 1; i <= 7; i++) {
        r = askText(g, now, U_SPOT, &XA);
        CHECK(r.action == GATE_ACTION_AUDIT);
        CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
        CHECK((g->serving != NULL) == (i < 7));
    }
    /* Both generations are gone, and so is everything the model knew. */
    CHECK(g->candidate == NULL && g->serving == NULL);
    CHECK(gateState(g) == GATE_STATE_COLD);
    CHECK_EQ_INT(g->stats.revocations, 1);
    CHECK_EQ_INT(g->trainer.updates, 0);
    CHECK_EQ_INT(g->snapshot_updates, 0);
    for (i = 0; i < g->trainer.nvec * g->trainer.dim; i++) nonzero += g->trainer.w[i] != 0.0f;
    for (i = 0; i < g->trainer.nvec; i++) nonzero += g->trainer.b[i] != 0.0f;
    CHECK_EQ_INT(nonzero, 0);
    /* It starts over like a new gate, except for the numbering. */
    r = askText(g, now, U_SPOT, &XA);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0);
    CHECK_EQ_INT(flagsOf(g, r.seq), GATE_OBS_AUDIT);
    for (i = 0; i < 12; i++) {
        CHECK(g->candidate == NULL);
        train(g, now, (i & 1) ? &XB : &XA, (int32_t)(i & 1));
    }
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 3);
    CHECK_EQ_INT(g->generations, 3);
    gateFree(g);
}

/* --------------------------- The rules of the final round ------------------
 * CHANGES.md section 9, item by item. Every number is derived in the file
 * comment or next to its use. */

/* PROMOTION compares like with like. A candidate takes over when its policy
 * answers more of its own proof requests than the policy the serving
 * generation has NOW answers of those same requests. The gate keeps, for
 * each proof request of the candidate, where the serving generation's own
 * guess put it (the paired counts: per row and threshold). Before, it kept
 * one number, what the serving generation would have answered when each
 * request was asked, and a serving generation whose thresholds had moved
 * down since was replaced by a candidate that answers less. */
TEST(promotion_like_with_like) {
    /* What generation 1 guessed for the proof requests of generation 2, by
     * the steps below. */
    static const servGuess seen[] = {
        {0, 0.6, 10}, {0, 0.99, 3}, {0, 0.6, 30}, {0, 0.6, 11}, {1, 0.8, 10}
    };
    gate *g = classGate(2, 5);
    gatePredictResult r;
    int64_t now = T0;
    gateEval ev;
    uint32_t i, t;

    for (i = 0; i < 5; i++) train(g, now, &XA, 0);
    /* Generation 1: four wrong guesses at 0.6 (the thresholds 0 and 1 of
     * label0), then 39 correct ones at 0.99 (the thresholds 0..19). The
     * thresholds 2..19 are proven by the 39th. To pay for four wrong
     * answers, 0 and 1 need 82 correct ones (file comment) and have 39.
     * Generation 1 answers label0 from threshold 2 (0.665). */
    CHECK_EQ_INT(needCorrect(ALPHA, log(640.0), 4), 82);
    for (i = 0; i < 4; i++) {
        ev = guesses(g, -1, 0.0, 0, 0.6);
        r = askGuess(g, now, U_PLAIN, &ev, &XA);
        CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
    }
    for (i = 0; i < 39; i++) {
        ev = guesses(g, -1, 0.0, 0, 0.99);
        r = askGuess(g, now, U_PLAIN, &ev, &XA);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
    }
    CHECK(g->serving != NULL && servingOf(g)->gen == 1 && servingOf(g)->policy[0] == 2);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 2);
    expectRegions(g, servingOf(g), 0, 0, 2, 43, 4);
    expectPaired(g, candidateOf(g), NULL, 0);

    /* Step 1. Ten spot checks that generation 1 does not answer (label0 at
     * 0.6 is under its threshold) and gets right; generation 2 says label1
     * and is wrong. */
    for (i = 0; i < 10; i++) {
        ev = guesses(g, 0, 0.6, 1, 0.7);
        r = askGuess(g, now, U_SPOT, &ev, &XA);
        CHECK(r.action == GATE_ACTION_ABSTAIN);
        CHECK_EQ_INT(flagsOf(g, r.seq), GATE_OBS_AUDIT | GATE_OBS_SERV | GATE_OBS_CAND);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
    }
    expectPaired(g, candidateOf(g), seen, 1);
    /* Step 2. Three that both answer (label0 at 0.99) and get right. */
    for (i = 0; i < 3; i++) {
        ev = guesses(g, 0, 0.99, 0, 0.99);
        r = askGuess(g, now, U_SPOT, &ev, &XA);
        CHECK(r.action == GATE_ACTION_AUDIT);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
    }
    expectPaired(g, candidateOf(g), seen, 2);
    /* Step 3. Thirty for which generation 1 says label0 at 0.6 and
     * generation 2 label0 at 0.99, both right. With the 30th the thresholds
     * 0 and 1 of generation 1 have their 82 correct answers (39 + 10 + 3 +
     * 30): from now on it answers label0 from 0.5. */
    for (i = 1; i <= 30; i++) {
        ev = guesses(g, 0, 0.6, 0, 0.99);
        r = askGuess(g, now, U_SPOT, &ev, &XA);
        CHECK(r.action == GATE_ACTION_ABSTAIN);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
        CHECK_EQ_INT(servingOf(g)->policy[0], i < 30 ? 2 : 0);
    }
    expectRegions(g, servingOf(g), 0, 0, 2, 86, 4);
    CHECK_EQ_INT(servingOf(g)->regions[0].certified_at, 86);
    CHECK_EQ_INT(servingOf(g)->regions[1].certified_at, 86);
    expectPaired(g, candidateOf(g), seen, 3);
    CHECK_EQ_INT(candidateOf(g)->regions[0].n, 33);
    CHECK_EQ_INT(candidateOf(g)->policy[0], -1);

    /* Step 4. Eleven more of the same; generation 1 answers them now (they
     * are spot checks of answers). With the eleventh generation 2 has its
     * 44 correct answers for label0: thresholds of its own, from 0.5. */
    for (i = 1; i <= 11; i++) {
        ev = guesses(g, 0, 0.6, 0, 0.99);
        r = askGuess(g, now, U_SPOT, &ev, &XA);
        CHECK(r.action == GATE_ACTION_AUDIT);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
        CHECK_EQ_INT(candidateOf(g)->policy[0], i < 11 ? -1 : 0);
        CHECK_EQ_INT(servingOf(g)->gen, 1);
    }
    /* Of its 54 proof requests generation 2 answers 44 (those for which it
     * said label0). Generation 1, with the policy it has now, answers all
     * 54: where it put each of them is in the paired counts, entry by
     * entry. 44 is not more than 54: no takeover. (Counted when each
     * request was asked, generation 1 answered 14 of them, the three of
     * step 2 and these eleven, and a candidate that answers less would
     * have replaced it.) */
    expectPaired(g, candidateOf(g), seen, 4);
    for (t = 0; t < T; t++) {
        CHECK_EQ_INT(candidateOf(g)->paired[0 * T + t], t < 2 ? 54 : (t < 20 ? 3 : 0));
        CHECK_EQ_INT(candidateOf(g)->paired[1 * T + t], 0);
    }
    CHECK_EQ_INT(candidateOf(g)->samples, 54);
    CHECK_EQ_INT(candidateOf(g)->regions[0].n, 44);
    CHECK(servingOf(g)->gen == 1 && candidateOf(g)->gen == 2);
    CHECK_EQ_INT(g->stats.promotions, 1);

    /* Step 5. Requests that only generation 2 answers: generation 1 says
     * label1, for which it has no threshold. Ten of them make it 54 against
     * 54: level is not more. The eleventh is one more, and generation 2
     * takes over. */
    for (i = 1; i <= 11; i++) {
        if (i == 11) {
            expectPaired(g, candidateOf(g), seen, 5);
            CHECK_EQ_INT(candidateOf(g)->paired[1 * T + 4], 10); /* 0.8: thresholds 0..4 */
            CHECK_EQ_INT(candidateOf(g)->paired[1 * T + 5], 0);
            CHECK_EQ_INT(candidateOf(g)->regions[0].n, 54);
        }
        ev = guesses(g, 1, 0.8, 0, 0.99);
        r = askGuess(g, now, U_SPOT, &ev, &XA);
        CHECK(r.action == GATE_ACTION_ABSTAIN);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
        CHECK_EQ_INT(servingOf(g)->gen, i < 11 ? 1 : 2);
    }
    CHECK_EQ_INT(g->stats.promotions, 2);
    CHECK_EQ_INT(servingOf(g)->samples, 65);
    CHECK_EQ_INT(servingOf(g)->regions[0].n, 55);
    CHECK_EQ_INT(servingOf(g)->policy[0], 0);
    /* The counts are zero again: they were about a generation that is gone. */
    expectPaired(g, servingOf(g), NULL, 0);
    CHECK_EQ_INT(servingOf(g)->budget, 0);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 3);
    expectPaired(g, candidateOf(g), NULL, 0);
    gateFree(g);
}

/* The comparison is over all labels the serving generation answers: what
 * it answers of the candidate's proof requests with one label counts like
 * what it answers with another. */
TEST(promotion_counts_every_label) {
    static const servGuess seen[] = {{0, 0.99, 44}, {1, 0.9, 5}, {0, 0.4, 6}};
    int64_t now = T0;
    gate *g = twoGenerations(now);
    gatePredictResult r;
    gateEval ev;
    uint32_t i;

    /* Generation 1 gets a threshold for label1 as well (39 correct answers
     * at 0.99; not spot checks, so nothing for generation 2). */
    for (i = 0; i < 39; i++) {
        ev = guesses(g, 1, 0.99, 1, 0.99);
        r = askGuess(g, now, U_PLAIN, &ev, &XB);
        CHECK_EQ_INT(flagsOf(g, r.seq), GATE_OBS_SERV);
        CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
    }
    CHECK(servingOf(g)->gen == 1 && servingOf(g)->policy[0] == 0 && servingOf(g)->policy[1] == 0);
    CHECK(candidateOf(g)->gen == 2 && candidateOf(g)->samples == 0);
    /* 44 spot checks both answer with label0: generation 2 has thresholds
     * for label0 with the 44th, and answers 44 of 44 like generation 1. */
    for (i = 0; i < 44; i++) {
        ev = guesses(g, 0, 0.99, 0, 0.99);
        r = askGuess(g, now, U_SPOT, &ev, &XA);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
    }
    CHECK(candidateOf(g)->policy[0] == 0 && candidateOf(g)->policy[1] == -1);
    /* Five that both give label1, at 0.9: generation 1 answers them,
     * generation 2 has no threshold for label1. 44 against 49. */
    for (i = 0; i < 5; i++) {
        ev = guesses(g, 1, 0.9, 1, 0.9);
        r = askGuess(g, now, U_SPOT, &ev, &XB);
        CHECK(r.action == GATE_ACTION_AUDIT);
        CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
    }
    expectPaired(g, candidateOf(g), seen, 2);
    CHECK_EQ_INT(candidateOf(g)->paired[0 * T + 0], 44);
    CHECK_EQ_INT(candidateOf(g)->paired[1 * T + 0], 5);
    CHECK_EQ_INT(servingOf(g)->gen, 1);
    /* Requests that only generation 2 answers (generation 1 says label0 at
     * 0.4, under every threshold). Five make it 49 against 49; the sixth is
     * one more. (Against the 44 of label0 alone, the first would do.) */
    for (i = 1; i <= 6; i++) {
        ev = guesses(g, 0, 0.4, 0, 0.99);
        r = askGuess(g, now, U_SPOT, &ev, &XA);
        CHECK(r.action == GATE_ACTION_ABSTAIN);
        if (i == 6) {
            expectPaired(g, candidateOf(g), seen, 2); /* 0.4 is counted nowhere */
            CHECK_EQ_INT(candidateOf(g)->regions[0].n, 49);
        }
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
        CHECK_EQ_INT(servingOf(g)->gen, i < 6 ? 1 : 2);
    }
    CHECK_EQ_INT(g->stats.promotions, 2);
    CHECK_EQ_INT(servingOf(g)->regions[0].n, 50);
    CHECK_EQ_INT(servingOf(g)->samples, 55);
    expectPaired(g, servingOf(g), NULL, 0);
    gateFree(g);
}

/* One answer can be proof for both generations. The comparison is with the
 * policy the serving generation has NOW: the answer counts for it as it
 * counts for the candidate, and then the two are compared. Here the answer
 * that completes the candidate's proof is also the one that lets the
 * serving generation answer every one of the candidate's proof requests.
 * (The header does not say in so many words which of the two comes first.
 * Counted for the candidate only, a snapshot that answers 44 of its 53
 * requests replaces one that answers all 53 from this very answer on:
 * what the paired counts are there to prevent.) */
TEST(promotion_counts_the_answer_for_both) {
    static const servGuess seen[] = {{0, 0.6, 53}};
    int late;

    /* late = 0: every answer in its turn. late = 1: the answer of the
     * first of the 44 requests arrives when proof has moved on without it,
     * and is the one that completes both proofs when it is set right. */
    for (late = 0; late < 2; late++) {
        gate *g = classGate(2, 5);
        gatePredictResult r, first;
        int64_t now = T0;
        gateEval ev;
        uint32_t i;

        for (i = 0; i < 5; i++) train(g, now, &XA, 0);
        /* Generation 1: five wrong guesses at 0.6 (the thresholds 0 and 1
         * of label0), then 39 correct ones at 0.99. It answers label0 from
         * threshold 2; to pay for five wrong answers the thresholds 0 and 1
         * need 92 correct ones (file comment) and have 39. */
        CHECK_EQ_INT(needCorrect(ALPHA, log(640.0), 5), 92);
        for (i = 0; i < 5; i++) {
            ev = guesses(g, -1, 0.0, 0, 0.6);
            r = askGuess(g, now, U_PLAIN, &ev, &XA);
            CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
        }
        for (i = 0; i < 39; i++) {
            ev = guesses(g, -1, 0.0, 0, 0.99);
            r = askGuess(g, now, U_PLAIN, &ev, &XA);
            CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
        }
        CHECK(g->serving != NULL && servingOf(g)->gen == 1 && servingOf(g)->policy[0] == 2);
        CHECK(g->candidate != NULL && candidateOf(g)->gen == 2);
        /* Nine spot checks: generation 1 says label0 at 0.6 (under its
         * threshold) and is right, generation 2 says label1 and is wrong. */
        for (i = 0; i < 9; i++) {
            ev = guesses(g, 0, 0.6, 1, 0.7);
            r = askGuess(g, now, U_SPOT, &ev, &XA);
            CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
        }
        /* 44 more: generation 1 the same, generation 2 label0 at 0.99, both
         * right. The 44th is the 92nd correct answer of generation 1 at the
         * thresholds 0 and 1 (39 + 9 + 44) and the 44th of generation 2,
         * which needs 44 to reach 1920. Generation 2 answers 44 of its 53
         * proof requests. Generation 1, which answered none of them until
         * now, answers all 53 from here on: it stays. */
        memset(&first, 0, sizeof(first));
        for (i = 1; i <= 44; i++) {
            ev = guesses(g, 0, 0.6, 0, 0.99);
            r = askGuess(g, now, U_SPOT, &ev, &XA);
            CHECK(r.action == GATE_ACTION_ABSTAIN);
            if (late && i == 1) {
                first = r;
                continue;
            }
            CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
            CHECK(g->serving != NULL);
            if (g->serving == NULL) break;
            CHECK_EQ_INT(servingOf(g)->gen, 1);
            if (servingOf(g)->gen != 1) break;
            CHECK_EQ_INT(servingOf(g)->policy[0], !late && i == 44 ? 0 : 2);
        }
        if (late) {
            /* Proof moves on without the first: a wrong guess of both, for
             * now. 43 correct answers after one wrong one prove nothing for
             * generation 2 (it would take 56), and generation 1 has six
             * wrong ones against 91. Its answer sets both right at once. */
            expectDue(g, now + GRACE);
            CHECK(servingOf(g)->gen == 1 && servingOf(g)->policy[0] == 2);
            CHECK(g->candidate != NULL && candidateOf(g)->gen == 2 && candidateOf(g)->policy[0] == -1);
            expectRegions(g, servingOf(g), 0, 0, 2, 97, 6);
            CHECK_EQ_INT(answer(g, now + GRACE + 50, first.seq, 0), 1);
            CHECK(g->serving != NULL);
            if (g->serving != NULL) {
                CHECK_EQ_INT(servingOf(g)->gen, 1);
                if (servingOf(g)->gen == 1) CHECK_EQ_INT(servingOf(g)->policy[0], 0);
            }
        }
        CHECK_EQ_INT(g->stats.promotions, 1);
        if (g->serving != NULL && servingOf(g)->gen == 1 && g->candidate != NULL) {
            expectRegions(g, servingOf(g), 0, 0, 2, 97, 5);
            CHECK_EQ_INT(servingOf(g)->regions[0].certified_at, 97);
            CHECK(candidateOf(g)->gen == 2 && candidateOf(g)->policy[0] == 0);
            CHECK(candidateOf(g)->samples == 53 && candidateOf(g)->regions[0].n == 44);
            expectPaired(g, candidateOf(g), seen, 1);
        }
        gateFree(g);
    }
}

/* BLIND: the run of missing answers is counted over requests in the order
 * they were asked. A request that ends unanswered is part of an outage only
 * if no answer of any request has arrived since it was asked; otherwise it
 * is a lost answer (a missing answer and a wrong guess all the same). */
TEST(lost_answers_are_not_an_outage) {
    gatePredictResult r;
    int64_t t = T0;
    uint32_t i;
    int k;
    gate *g;

    /* Requests given up together after a lull, with answers in between. 50
     * requests at one moment: number 11 and number 31 are answered at once,
     * nobody answers the others. Then nothing happens for five times the
     * feedback timeout, and the time of all 48 is up in one call. Those
     * asked before the last answer arrived have seen an answer arrive: 29
     * lost answers. The 19 asked after it are a run of 19. (Counted from
     * the last answer, whatever was asked before it, the 48 were a run and
     * the 32nd made the gate blind.) */
    g = scoreGate();
    memset(&r, 0, sizeof(r));
    for (i = 1; i <= 50; i++) {
        r = ask(g, t, U_PLAIN, 0.9);
        CHECK_EQ_INT(r.seq, i);
        if (i == 11 || i == 31) CHECK_EQ_INT(answer(g, t, r.seq, 1), 1);
    }
    CHECK_EQ_INT(g->answer_mark, 32);
    CHECK_EQ_INT(g->nanswered, 2);
    gateMaintain(g, t + 5 * TIMEOUT);
    inv(g);
    CHECK_EQ_INT(g->stats.missing, 48);
    CHECK_EQ_INT(g->missing_run, 19);
    CHECK_EQ_INT(g->blind, 0);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 1);
    CHECK(candidateOf(g)->samples == 50 && candidateOf(g)->errors == 48);
    CHECK_EQ_INT(g->npending, 0);
    /* The run goes on from there: twelve more make 31, the 13th is the
     * 32nd. */
    t = loseAnswers(g, t + 5 * TIMEOUT, 12, U_PLAIN);
    CHECK_EQ_INT(g->missing_run, 31);
    CHECK_EQ_INT(g->blind, 0);
    t = loseAnswers(g, t, 1, U_PLAIN);
    CHECK_EQ_INT(g->blind, 1);
    CHECK_EQ_INT(g->stats.missing, 61);
    gateFree(g);

    /* A burst asked at one instant and lost: 100 requests in the same
     * millisecond, no answer for any of them. k = 0: no other request is
     * answered either. That is an outage: the 32nd makes the gate blind,
     * and the 68 behind it are proof for nothing any more. k = 1, 2: ten
     * milliseconds later one more request is asked and answered. Then the
     * hundred are lost answers, all of them, whether their time is up while
     * they are still in the queue (1) or after proof has moved on without
     * them (2). */
    for (k = 0; k < 3; k++) {
        g = scoreGate();
        t = T0;
        for (i = 0; i < 100; i++) CHECK(ask(g, t, U_PLAIN, 0.9).seq != 0);
        if (k) {
            r = ask(g, t + 10, U_PLAIN, 0.9);
            CHECK_EQ_INT(answer(g, t + 10, r.seq, 1), 1);
        }
        if (k == 2) {
            expectDue(g, t + GRACE);
            CHECK_EQ_INT(flagsOf(g, 1), GATE_OBS_CAND | GATE_OBS_PROVISIONAL);
            CHECK_EQ_INT(flagsOf(g, 100), GATE_OBS_CAND | GATE_OBS_PROVISIONAL);
            CHECK_EQ_INT(g->nevidence, 0);
        }
        gateMaintain(g, t + TIMEOUT);
        inv(g);
        if (k) {
            CHECK_EQ_INT(g->stats.missing, 100);
            CHECK_EQ_INT(g->missing_run, 0);
            CHECK_EQ_INT(g->blind, 0);
            CHECK(g->candidate != NULL && candidateOf(g)->samples == 101 &&
                  candidateOf(g)->errors == 100);
            CHECK_EQ_INT(g->stats.dropped, 0);
        } else {
            CHECK_EQ_INT(g->stats.missing, 32);
            CHECK_EQ_INT(g->blind, 1);
            CHECK_EQ_INT(g->stats.dropped, 68);
            CHECK(g->candidate == NULL);
        }
        CHECK_EQ_INT(g->npending, 0);
        gateFree(g);
    }

    /* A real outage that begins after an answer. Five requests are lost,
     * the sixth is answered, and from then on nothing is, one request a
     * millisecond. The five are lost answers. The requests asked after the
     * answer are a run from the first: the 32nd of them, request 38, ends
     * it. */
    g = scoreGate();
    t = T0;
    for (i = 1; i <= 6; i++) r = ask(g, t, U_PLAIN, 0.9);
    CHECK_EQ_INT(answer(g, t, r.seq, 1), 1);
    for (i = 7; i <= 38; i++) CHECK_EQ_INT(ask(g, t + i, U_PLAIN, 0.9).seq, i);
    gateMaintain(g, t + TIMEOUT);
    inv(g);
    CHECK_EQ_INT(g->stats.missing, 5);
    CHECK_EQ_INT(g->missing_run, 0);
    for (i = 7; i <= 38; i++) {
        gateMaintain(g, t + i + TIMEOUT);
        inv(g);
        CHECK_EQ_INT(g->stats.missing, 5 + (i - 6));
        CHECK_EQ_INT(g->blind, i == 38);
        CHECK_EQ_INT(g->missing_run, i < 38 ? i - 6 : 0);
    }
    CHECK(g->candidate == NULL && g->generations == 1);
    gateFree(g);

    /* A partial outage: 19 answers in 20 are lost for four seconds of one
     * request a millisecond (of every hundred requests the first five are
     * answered, at once). 95 lost answers in a row, forty times over, and
     * never an outage: by the time one of them is given up, a second
     * later, answers have arrived. The gate goes on testing the snapshot
     * it has and uses no snapshot number. (Counted from the last answer,
     * every hundred held a run of 95: the gate went blind forty times and
     * each time the next answer cost a snapshot number.) */
    g = scoreGate();
    t = T0;
    for (i = 0; i < 4005; i++) {
        r = ask(g, t + i, U_PLAIN, 0.9);
        if (r.seq == 0) break;
        if (i % 100 < 5) CHECK_EQ_INT(answer(g, t + i, r.seq, 1), 1);
        if (g->blind || g->missing_run || g->generations != 1) break;
    }
    CHECK_EQ_INT(i, 4005);
    gateMaintain(g, t + 4004 + TIMEOUT);
    inv(g);
    CHECK_EQ_INT(g->stats.missing, 3800);
    CHECK_EQ_INT(g->stats.learned, 205);
    CHECK_EQ_INT(g->missing_run, 0);
    CHECK_EQ_INT(g->blind, 0);
    CHECK_EQ_INT(g->generations, 1);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 1);
    CHECK(candidateOf(g)->samples == 4005 && candidateOf(g)->errors == 3800);
    CHECK_EQ_INT(g->stats.full, 0);
    CHECK_EQ_INT(g->stats.revocations, 0);
    gateFree(g);

    /* The mark is a request number, and request numbers go on through a
     * reset and a recovery: what is asked after one has seen no answer,
     * whatever was answered before. 32 such requests without an answer are
     * an outage like any other. */
    for (k = 0; k < 2; k++) {
        g = scoreGate();
        t = T0;
        feed(g, t, U_PLAIN, 0.9, 1);
        for (i = 0; i < 3; i++) CHECK(ask(g, t, U_PLAIN, 0.9).seq != 0);
        CHECK_EQ_INT(g->answer_mark, 2);
        if (k) gateReset(g, t, EPOCH2); else gateRecover(g, t, EPOCH2);
        inv(g);
        CHECK_EQ_INT(g->next_seq, 5);
        CHECK(g->answer_mark <= g->next_seq);
        t = loseAnswers(g, t, 31, U_PLAIN);
        CHECK_EQ_INT(g->missing_run, 31);
        CHECK_EQ_INT(g->blind, 0);
        t = loseAnswers(g, t, 1, U_PLAIN);
        CHECK_EQ_INT(g->blind, 1);
        CHECK(g->candidate == NULL);
        gateFree(g);
    }
}

/* THE BUDGET of a snapshot is used up by its proof and by the requests it
 * could have been tested on had it not guessed a label that is never
 * answered: asked of it while it was the candidate, at a moment when its
 * requests are proof (nothing serving, or a spot check), answered, and
 * with a teacher's label that is one the gate may answer. */
TEST(missed_requests) {
    gatePredictResult r, early, late, open;
    gateConfig cfg;
    int64_t t = T0;
    gateEval ev;
    uint32_t i;
    int last;
    gate *g;

    classConfig(&cfg, 3, 5);
    cfg.class_defer[2] = 1;
    g = create(&cfg);
    /* Asked before there is a snapshot: nothing to do with any. */
    early = askText(g, t, U_PLAIN, &XA);
    for (i = 0; i < 5; i++) train(g, t, &XA, 0);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 1);
    CHECK_EQ_INT(candidateOf(g)->budget, 780);
    CHECK_EQ_INT(answer(g, t, early.seq, 0), 1);
    CHECK_EQ_INT(candidateOf(g)->missed, 0);

    /* Nothing serves: every request would be proof for generation 1,
     * whatever the draw. It says label2. */
    ev = guesses(g, -1, 0.0, 2, 0.9);
    r = askGuess(g, t, U_PLAIN, &ev, &XA);
    CHECK_EQ_INT(answer(g, t, r.seq, 0), 1); /* label0: it could have been tested on this one */
    CHECK_EQ_INT(candidateOf(g)->missed, 1);
    r = askGuess(g, t, U_SPOT, &ev, &XA);
    CHECK_EQ_INT(answer(g, t, r.seq, 1), 1); /* label1 */
    CHECK_EQ_INT(candidateOf(g)->missed, 2);
    CHECK_EQ_INT(answer(g, t, r.seq, 1), 0); /* an answer is used once */
    CHECK_EQ_INT(gateLearn(g, t, EPOCH, g->next_seq, 0), 0);
    CHECK_EQ_INT(candidateOf(g)->missed, 2);
    r = askGuess(g, t, U_PLAIN, &ev, &XC);
    CHECK_EQ_INT(answer(g, t, r.seq, 2), 1); /* label2: nothing the gate may answer */
    CHECK_EQ_INT(candidateOf(g)->missed, 2);
    /* A guess of a label that is answered is proof, right or wrong. */
    ev = guesses(g, -1, 0.0, 0, 0.9);
    r = askGuess(g, t, U_PLAIN, &ev, &XA);
    CHECK_EQ_INT(answer(g, t, r.seq, 1), 1);
    CHECK(candidateOf(g)->missed == 2 && candidateOf(g)->samples == 1 &&
          candidateOf(g)->errors == 1);
    /* No answer: nothing is known about the request. (It was kept for its
     * label only: forgotten, not a missing answer.) */
    ev = guesses(g, -1, 0.0, 2, 0.9);
    open = askGuess(g, t, U_PLAIN, &ev, &XA);
    gateMaintain(g, t + TIMEOUT);
    inv(g);
    CHECK(gateFindObservation(g, open.seq) == NULL);
    CHECK(candidateOf(g)->missed == 2 && g->stats.missing == 0 && g->stats.dropped == 1);
    t += TIMEOUT;

    /* Generation 1 comes to serve: 39 correct guesses of label0 at 0.99
     * (its one wrong guess at 0.9 leaves the thresholds 0..9 behind: it
     * answers from threshold 10, 0.918). Generation 2 is under test, and
     * its proof is the spot checks now: only those can be missed. */
    for (i = 0; i < 39; i++) {
        ev = guesses(g, -1, 0.0, 0, 0.99);
        r = askGuess(g, t, U_PLAIN, &ev, &XA);
        CHECK_EQ_INT(answer(g, t, r.seq, 0), 1);
    }
    CHECK(g->serving != NULL && servingOf(g)->gen == 1 && servingOf(g)->policy[0] == 10);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 2);
    CHECK_EQ_INT(candidateOf(g)->budget, 880);
    CHECK_EQ_INT(candidateOf(g)->missed, 0);
    /* Not a spot check: both say label2 (kept for training only). */
    ev = guesses(g, 2, 0.9, 2, 0.9);
    r = askGuess(g, t, U_PLAIN, &ev, &XA);
    CHECK_EQ_INT(flagsOf(g, r.seq), 0);
    CHECK_EQ_INT(answer(g, t, r.seq, 0), 1);
    CHECK_EQ_INT(candidateOf(g)->missed, 0);
    /* Not a spot check either, though proof for generation 1 (it says
     * label1, which it does not answer yet). */
    ev = guesses(g, 1, 0.8, 2, 0.9);
    r = askGuess(g, t, U_PLAIN, &ev, &XA);
    CHECK_EQ_INT(flagsOf(g, r.seq), GATE_OBS_SERV);
    CHECK_EQ_INT(answer(g, t, r.seq, 0), 1);
    CHECK_EQ_INT(candidateOf(g)->missed, 0);
    /* Spot checks: of something generation 1 does not answer, */
    ev = guesses(g, 2, 0.9, 2, 0.9);
    r = askGuess(g, t, U_SPOT, &ev, &XA);
    CHECK_EQ_INT(flagsOf(g, r.seq), GATE_OBS_AUDIT);
    CHECK_EQ_INT(answer(g, t, r.seq, 0), 1);
    CHECK_EQ_INT(candidateOf(g)->missed, 1);
    /* ... of an answer of generation 1, */
    ev = guesses(g, 0, 0.99, 2, 0.9);
    r = askGuess(g, t, U_SPOT, &ev, &XA);
    CHECK(r.action == GATE_ACTION_AUDIT);
    CHECK_EQ_INT(answer(g, t, r.seq, 1), 1);
    CHECK_EQ_INT(candidateOf(g)->missed, 2);
    /* ... and one the teacher calls label2: not missed. */
    r = askGuess(g, t, U_SPOT, &ev, &XA);
    CHECK_EQ_INT(answer(g, t, r.seq, 2), 1);
    CHECK_EQ_INT(candidateOf(g)->missed, 2);
    CHECK(candidateOf(g)->samples == 0 && g->serving != NULL);

    /* A late answer counts when it arrives, once: proof for generation 1
     * had moved on without this spot check. */
    late = askGuess(g, t, U_SPOT, &ev, &XA);
    ev = guesses(g, 0, 0.99, 0, 0.99);
    r = askGuess(g, t, U_SPOT, &ev, &XA);
    CHECK_EQ_INT(answer(g, t, r.seq, 0), 1);
    expectDue(g, t + GRACE);
    CHECK_EQ_INT(flagsOf(g, late.seq) & GATE_OBS_PROVISIONAL, GATE_OBS_PROVISIONAL);
    CHECK_EQ_INT(candidateOf(g)->missed, 2);
    CHECK_EQ_INT(answer(g, t + GRACE + 1, late.seq, 0), 1);
    CHECK_EQ_INT(candidateOf(g)->missed, 3);
    CHECK_EQ_INT(candidateOf(g)->samples, 1);

    /* Asked of generation 2 while it was under test, answered when it
     * serves: it is no candidate any more, and generation 3 was never
     * asked. (Generation 2 takes over with 44 correct spot checks, 43 of
     * them for requests that generation 1 does not answer.) */
    ev = guesses(g, 2, 0.9, 2, 0.9);
    open = askGuess(g, t + GRACE + 1, U_SPOT, &ev, &XA);
    for (i = 0; i < 43; i++) {
        ev = guesses(g, 1, 0.8, 0, 0.99);
        r = askGuess(g, t + GRACE + 1, U_SPOT, &ev, &XA);
        CHECK_EQ_INT(answer(g, t + GRACE + 1, r.seq, 0), 1);
    }
    CHECK(servingOf(g)->gen == 2 && g->candidate != NULL && candidateOf(g)->gen == 3);
    CHECK_EQ_INT(answer(g, t + GRACE + 1, open.seq, 0), 1);
    CHECK_EQ_INT(candidateOf(g)->missed, 0);
    CHECK_EQ_INT(g->stats.retired, 0);
    gateFree(g);

    /* The snapshot that only knows a label that is never answered. Twelve
     * examples, all label2: the frozen model says label2 to everything. It
     * can prove nothing; before, it was never dropped either, and the gate
     * tested it for ever. Now every answer that says label0 counts: at the
     * 780th (10 x 39 for each of the two labels that are answered) it
     * goes, and the next snapshot, of a model that has learned from those
     * answers, is tried at once. */
    classConfig(&cfg, 3, 12);
    cfg.class_defer[2] = 1;
    g = create(&cfg);
    t = T0;
    for (i = 0; i < 12; i++) train(g, t, &XC, 2);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 1);
    CHECK_EQ_INT(candidateOf(g)->budget, 780);
    CHECK_EQ_INT(gateModelPredict(&candidateOf(g)->model, &XA, NULL, NULL), 2);
    for (i = 1; i <= 780; i++) {
        r = askText(g, t, (i & 1) ? U_PLAIN : U_SPOT, &XA);
        CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0);
        CHECK_EQ_INT(flagsOf(g, r.seq) & GATE_OBS_EVIDENCE, 0);
        CHECK_EQ_INT(answer(g, t, r.seq, 0), 1);
        CHECK_EQ_INT(candidateOf(g)->gen, i < 780 ? 1 : 2);
        if (i < 780) CHECK(candidateOf(g)->missed == i && candidateOf(g)->samples == 0);
    }
    CHECK_EQ_INT(g->stats.retired, 1);
    CHECK_EQ_INT(g->generations, 2);
    CHECK(candidateOf(g)->budget == 880 && candidateOf(g)->missed == 0 &&
          candidateOf(g)->samples == 0);
    CHECK_EQ_INT(candidateOf(g)->model.updates, 12 + 780);
    /* That one has learned what XA is: 44 correct answers, and the gate
     * answers. */
    CHECK_EQ_INT(gateModelPredict(&candidateOf(g)->model, &XA, NULL, NULL), 0);
    for (i = 1; i <= 44; i++) {
        feedText(g, t, U_PLAIN, &XA, 0);
        CHECK_EQ_INT(g->serving != NULL, i == 44);
    }
    r = askText(g, t, U_PLAIN, &XA);
    CHECK(r.action == GATE_ACTION_ACCEPT && r.label == 0);
    gateFree(g);

    /* Proof and missed requests add up: 700 wrong guesses of a label that
     * is answered and 79 missed requests leave the snapshot in place; one
     * more of either kind is its last. */
    for (last = 0; last < 2; last++) {
        classConfig(&cfg, 3, 5);
        cfg.class_defer[2] = 1;
        g = create(&cfg);
        for (i = 0; i < 5; i++) train(g, t, &XA, 0);
        for (i = 0; i < 779; i++) {
            ev = guesses(g, -1, 0.0, i < 700 ? 0 : 2, 0.9);
            r = askGuess(g, t, U_PLAIN, &ev, &XA);
            CHECK_EQ_INT(answer(g, t, r.seq, 1), 1);
        }
        CHECK(candidateOf(g)->gen == 1 && candidateOf(g)->samples == 700 &&
              candidateOf(g)->missed == 79);
        CHECK_EQ_INT(g->stats.retired, 0);
        ev = guesses(g, -1, 0.0, last ? 0 : 2, 0.9);
        r = askGuess(g, t, U_PLAIN, &ev, &XA);
        CHECK_EQ_INT(answer(g, t, r.seq, 1), 1);
        CHECK_EQ_INT(candidateOf(g)->gen, 2);
        CHECK_EQ_INT(g->stats.retired, 1);
        gateFree(g);
    }
}

/* LIMITS. No request number, snapshot number or clock value goes beyond
 * its limit (GATE_COUNT_MAX, GATE_MAX_CLOCK_MS): the gate stops handing
 * them out. Only a payload can put a gate right at a limit; here the
 * fields are set as such a payload would leave them. (Built with the
 * codec, every inv() and every request below also takes the gate through
 * its encoding and back.) */
TEST(limits) {
    const int64_t top = GATE_MAX_CLOCK_MS;
    gatePredictResult r, r1, r2;
    gateConfig cfg;
    gateSparse x;
    gateEval ev;
    uint64_t before;
    char err[128];
    uint32_t i, need;
    gate *g;

    /* Request numbers. Two are left. */
    g = scoreGate();
    g->next_seq = GATE_COUNT_MAX - 2;
    inv(g);
    CHECK(!gateFull(g));
    r1 = ask(g, T0, U_PLAIN, 0.9);
    r2 = ask(g, T0, U_SPOT, 0.9);
    CHECK(r1.seq == GATE_COUNT_MAX - 2 && r2.seq == GATE_COUNT_MAX - 1);
    CHECK(g->next_seq == GATE_COUNT_MAX);
    /* Out of numbers: full, to every request, as if 4096 were waiting. */
    CHECK(gateFull(g));
    CHECK_EQ_INT(gateUnanswered(g), 2);
    before = fingerprint(g, SKIP_REQUESTS);
    for (i = 0; i < 3; i++) {
        r = ask(g, T0, i ? U_SPOT : U_PLAIN, 0.9);
        CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq == 0);
    }
    CHECK(fingerprint(g, SKIP_REQUESTS) == before);
    CHECK_EQ_INT(g->stats.full, 3);
    CHECK(g->next_seq == GATE_COUNT_MAX && g->npending == 2);
    /* The requests that got a number are answered as ever. */
    CHECK_EQ_INT(answer(g, T0, r2.seq, 1), 1);
    CHECK(g->answer_mark == GATE_COUNT_MAX);
    CHECK_EQ_INT(answer(g, T0, r1.seq, 1), 1);
    CHECK_EQ_INT(candidateOf(g)->samples, 2);
    CHECK_EQ_INT(gateLearn(g, T0, EPOCH, GATE_COUNT_MAX, 1), 0);
    CHECK(gateFull(g) && g->npending == 0);
    /* Numbers are not handed out twice, whatever is forgotten. */
    gateRecover(g, T0, EPOCH2);
    inv(g);
    CHECK(g->next_seq == GATE_COUNT_MAX && gateFull(g));
    gateReset(g, T0, EPOCH3);
    inv(g);
    CHECK(g->next_seq == GATE_COUNT_MAX && gateFull(g));
    CHECK(ask(g, T0, U_PLAIN, 0.9).seq == 0);
    gateFree(g);
    /* A classifier: no clues are wanted, none are taken. GADD needs no
     * number. */
    g = trainedGate(12);
    g->next_seq = GATE_COUNT_MAX;
    inv(g);
    gateMaintain(g, T0);
    CHECK(gateEvaluateFor(g, &XA, 0.0, U_PLAIN, &ev) == GATE_OK);
    CHECK_EQ_INT(gatePredictNeedsFeatures(g, U_PLAIN, &ev), 0);
    if (gateSparseCopy(&x, &XA) != GATE_OK) die("out of memory");
    CHECK(gatePredict(g, T0, U_PLAIN, &ev, &x, &r) == GATE_OK);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq == 0 && x.n == 1 && x.idx != NULL);
    gateSparseFree(&x);
    CHECK(g->npending == 0 && g->next_seq == GATE_COUNT_MAX && g->stats.full == 1);
    train(g, T0, &XA, 0);
    CHECK_EQ_INT(g->trainer.updates, 13);
    gateFree(g);

    /* Snapshot numbers. One is left: it is tested and promoted like any
     * other. Its bar is log(2^62 (2^62 + 1)) + log(32 / 0.2), which is 124
     * log 2 + log 160 to the last digit of a double: 456 correct answers. */
    g = scoreGate();
    g->generations = GATE_COUNT_MAX - 1;
    inv(g);
    gateRecover(g, T0, EPOCH2);
    inv(g);
    CHECK(g->generations == GATE_COUNT_MAX);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == GATE_COUNT_MAX);
    CHECK_NEAR(candidateOf(g)->log_threshold, 124.0 * log(2.0) + log(160.0), 1e-9);
    need = needCorrect(ALPHA, 124.0 * log(2.0) + log(160.0), 0);
    CHECK_EQ_INT(need, 456);
    for (i = 1; i <= need; i++) {
        feed(g, T0, U_PLAIN, 0.9, 1);
        CHECK_EQ_INT(g->serving != NULL, i == need);
    }
    CHECK(servingOf(g)->gen == GATE_COUNT_MAX && g->candidate == NULL);
    CHECK(ask(g, T0, U_PLAIN, 0.9).action == GATE_ACTION_ACCEPT);
    /* No snapshot is taken after it: not when one would be due, */
    gateRecover(g, T0, EPOCH3);
    inv(g);
    CHECK(g->candidate == NULL && g->serving == NULL && g->generations == GATE_COUNT_MAX);
    CHECK(gateState(g) == GATE_STATE_COLD);
    CHECK_EQ_INT(gateMaintenanceDue(g, T0 + 5), 0);
    expectIdle(g, T0 + 5);
    /* ... and a score gate with nothing to test records nothing. */
    r = ask(g, T0 + 5, U_SPOT, 0.9);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq == 0);
    gateReset(g, T0 + 5, EPOCH);
    inv(g);
    CHECK(g->candidate == NULL && g->generations == GATE_COUNT_MAX);
    gateFree(g);
    /* A classifier goes on learning, and that is all. */
    g = classGate(2, 3);
    g->generations = GATE_COUNT_MAX;
    for (i = 0; i < 10; i++) train(g, T0, &XA, 0);
    CHECK(g->candidate == NULL && g->generations == GATE_COUNT_MAX && g->trainer.updates == 10);
    CHECK_EQ_INT(gateMaintenanceDue(g, T0), 0);
    feedText(g, T0, U_PLAIN, &XA, 0);
    CHECK_EQ_INT(g->trainer.updates, 11);
    CHECK(gateState(g) == GATE_STATE_COLD);
    gateFree(g);

    /* The clock. Whatever a caller hands in, the gate holds a value between
     * 0 and GATE_MAX_CLOCK_MS: when it is created, */
    baseConfig(&cfg);
    if (gateConfigValidate(&cfg, err, sizeof(err)) != GATE_OK) die(err);
    g = gateCreate(&cfg, LINEAGE, EPOCH, INT64_MAX);
    if (g == NULL) die("gateCreate failed");
    CHECK(g->created_ms == top && g->now_ms == top && candidateOf(g)->frozen_ms == top);
    inv(g);
    gateFree(g);
    g = gateCreate(&cfg, LINEAGE, EPOCH, INT64_MIN);
    if (g == NULL) die("gateCreate failed");
    gateConfigFree(&cfg);
    CHECK(g->created_ms == 0 && g->now_ms == 0 && candidateOf(g)->frozen_ms == 0);
    inv(g);
    /* ... by every function that takes one, */
    r1 = ask(g, -5, U_PLAIN, 0.9);
    CHECK(r1.seq != 0 && gateFindObservation(g, r1.seq)->created_ms == 0);
    CHECK_EQ_INT(g->now_ms, 0);
    /* (from the first moment to beyond the last in one step: the request
     * asked at 0 is as old as a request gets, and its time is up) */
    CHECK_EQ_INT(gateMaintenanceDue(g, INT64_MAX), 1);
    CHECK_EQ_INT(gateMaintenanceDue(g, top - 1), 1);
    CHECK_EQ_INT(gateMaintenanceDue(g, TIMEOUT - 1), 0);
    gateMaintain(g, INT64_MAX);
    inv(g);
    CHECK_EQ_INT(g->now_ms, top);
    CHECK_EQ_INT(g->stats.missing, 1);
    r1 = ask(g, INT64_MAX, U_PLAIN, 0.9);
    CHECK(r1.seq != 0 && gateFindObservation(g, r1.seq)->created_ms == top);
    CHECK_EQ_INT(gateMaintenanceDue(g, INT64_MAX), 0);
    CHECK_EQ_INT(gateMaintenanceDue(g, INT64_MIN), 0);
    CHECK_EQ_INT(answer(g, INT64_MAX, r1.seq, 1), 1);
    CHECK_EQ_INT(gateLearn(g, INT64_MIN, EPOCH, r1.seq, 1), 0);
    gatePause(g, INT64_MAX);
    gateResume(g, INT64_MIN);
    CHECK_EQ_INT(g->now_ms, top);
    /* ... and by the two that set it. */
    gateRecover(g, INT64_MAX, EPOCH2);
    inv(g);
    CHECK(g->now_ms == top && candidateOf(g)->frozen_ms == top);
    gateReset(g, INT64_MIN, EPOCH3);
    inv(g);
    CHECK(g->now_ms == 0 && candidateOf(g)->frozen_ms == 0);
    gateReset(g, top + 1, EPOCH);
    inv(g);
    CHECK(g->now_ms == top && candidateOf(g)->gen == 4);
    /* A promotion at the last moment (generation 4: 47 correct answers
     * reach 3200). */
    for (i = 1; i <= 47; i++) {
        feed(g, INT64_MAX, U_PLAIN, 0.9, 1);
        CHECK_EQ_INT(g->serving != NULL, i == 47);
    }
    CHECK_EQ_INT(servingOf(g)->promoted_ms, top);
    gateFree(g);
    g = classGate(2, 3);
    train(g, INT64_MAX, &XA, 0);
    CHECK_EQ_INT(g->now_ms, top);
    train(g, INT64_MIN, &XA, 0);
    train(g, INT64_MAX, &XA, 0);
    CHECK(g->candidate != NULL && candidateOf(g)->frozen_ms == top);
    inv(g);
    gateFree(g);
}

/* The other counters are not stopped by the core: the encoding writes one
 * that is beyond the limit as the limit, and whatever the core made of a
 * gate that decoded must decode again. Every counter of a gate that
 * answers is put where a payload may hold it, at GATE_COUNT_MAX, and the
 * gate goes on. In the build of the Makefile this checks the invariants;
 * with the codec, inv() checks what the sentence above says. */
TEST(counters_at_their_limit) {
    const uint64_t top = GATE_COUNT_MAX;
    int64_t t = T0;
    gatePredictResult r;
    gateGeneration *gen;
    gateCounters *st;
    gateEval ev;
    uint32_t i;
    gate *g;
    int kind;

    codec_exact = 0;
    for (kind = 0; kind < 2; kind++) {
        g = kind ? twoGenerations(t) : activeScoreGate(t);
        st = &g->stats;
        /* Requests are replies: ACCEPT, AUDIT or ABSTAIN. */
        st->accepted = top / 2;
        st->audited = st->abstained = top / 4;
        st->requests = top;
        st->full = top / 4;
        st->observations = st->learned = st->ignored = st->missing = st->dropped = top;
        st->promotions = st->revocations = st->retired = top;
        gen = g->serving;
        gen->samples = gen->audits = gen->audit_errors = gen->missed = top;
        gen->errors = top / 2;
        for (i = 0; i < g->nrows * T; i++) {
            if (gen->regions[i].n == 0) continue;
            gen->regions[i].n = top;
            gen->regions[i].errors = top / 2;
        }
        gen->drift[0].asked = gen->drift[0].errors = top;
        if (kind) {
            g->trainer.updates = g->snapshot_updates = top;
            gen = g->candidate;
            gen->samples = gen->errors = gen->missed = top;
            for (i = 0; i < T; i++) gen->paired[i] = top;
            for (i = 0; i < T; i++) gen->regions[i].n = gen->regions[i].errors = top;
        }
        inv(g);

        /* An answer of its own, a spot check that agrees, one that
         * disagrees, one that is lost, a request it does not answer, an
         * answer nobody asked for. */
        if (kind) {
            ev = guesses(g, 0, 0.99, 0, 0.99);
            r = askGuess(g, t, U_PLAIN, &ev, &XA);
            CHECK(r.action == GATE_ACTION_ACCEPT);
            for (i = 0; i < 3; i++) {
                r = askGuess(g, t, U_SPOT, &ev, &XA);
                CHECK(r.action == GATE_ACTION_AUDIT);
                CHECK(spotOf(g, r.seq) == top + 1 + i);
                if (i < 2) CHECK_EQ_INT(answer(g, t, r.seq, (int32_t)i), 1);
            }
            ev = guesses(g, 1, 0.8, 1, 0.8);
            r = askGuess(g, t, U_PLAIN, &ev, &XB);
            CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0);
            train(g, t, &XB, 1);
        } else {
            CHECK(ask(g, t, U_PLAIN, 0.9).action == GATE_ACTION_ACCEPT);
            for (i = 0; i < 3; i++) {
                r = ask(g, t, U_SPOT, 0.9);
                CHECK(r.action == GATE_ACTION_AUDIT);
                CHECK(spotOf(g, r.seq) == top + 1 + i);
                if (i < 2) CHECK_EQ_INT(answer(g, t, r.seq, (int32_t)(1 - i)), 1);
            }
        }
        CHECK_EQ_INT(gateLearn(g, t, EPOCH, 1, 0), 0);
        inv(g);
        gateMaintain(g, t + TIMEOUT);
        inv(g);
        /* It counted on. */
        CHECK(st->requests > top && st->learned > top && st->ignored > top && st->missing > top);
        CHECK(g->serving != NULL && g->serving->drift[0].asked == top + 3);
        CHECK(g->serving->samples > top && g->serving->audits > top);
        gateFree(g);
    }
    codec_exact = 1;
}

/* gateMaintain(). What a gate decoded from a payload may hold, and the core
 * never leaves behind: a request at the head of the queue that is proof for
 * a generation that is gone. When its time is up it is forgotten: it is not
 * a missing answer and not the beginning of a run. */
TEST(dead_queue_head_is_dropped) {
    gateConfig cfg;
    gateObs *o[2];
    int i, classifier;
    gate *g;

    for (classifier = 0; classifier < 2; classifier++) {
        if (classifier) classConfig(&cfg, 2, 12); else baseConfig(&cfg);
        if (gateConfigValidate(&cfg, NULL, 0) != GATE_OK) die("invalid config");
        g = gateAllocate(&cfg);
        if (g == NULL) die("out of memory");
        gateConfigFree(&cfg);
        g->epoch = EPOCH;
        g->created_ms = T0;
        g->now_ms = T0 + 1;
        g->generations = 1; /* there was a generation 1 */
        for (i = 0; i < 2; i++) {
            o[i] = gateObsAllocate();
            if (o[i] == NULL) die("out of memory");
            o[i]->seq = (uint64_t)i + 1;
            o[i]->created_ms = T0 + i;
            o[i]->flags = GATE_OBS_CAND;
            o[i]->cand_gen = 1;
            o[i]->cand_label = classifier ? 0 : -1;
            o[i]->cand_score = 0.9;
            if (classifier && gateSparseCopy(&o[i]->x, &XA) != GATE_OK) die("out of memory");
        }
        g->next_seq = 3;
        gateObsLink(g, o[0]);
        gateObsLink(g, o[1]);
        inv(g);
        CHECK_EQ_INT(g->nevidence, 2);
        /* The time of the first is up, not yet that of the second. */
        CHECK_EQ_INT(gateMaintenanceDue(g, T0 + TIMEOUT), 1);
        gateMaintain(g, T0 + TIMEOUT);
        inv(g);
        CHECK_EQ_INT(g->stats.missing, 0);
        CHECK_EQ_INT(g->missing_run, 0);
        CHECK_EQ_INT(g->blind, 0);
        CHECK_EQ_INT(g->nevidence, 0);
        if (classifier) {
            /* The second is kept for its label. */
            CHECK_EQ_INT(g->stats.dropped, 1);
            CHECK_EQ_INT(g->npending, 1);
            CHECK_EQ_INT(flagsOf(g, 2), 0);
            CHECK_EQ_INT(answer(g, T0 + TIMEOUT, 2, 0), 1);
            CHECK_EQ_INT(g->trainer.updates, 1);
        } else {
            /* A score gate has no use for the second, and starts its next
             * test, which neither was asked of. */
            CHECK_EQ_INT(g->stats.dropped, 2);
            CHECK(g->candidate != NULL && candidateOf(g)->gen == 2 && candidateOf(g)->samples == 0);
        }
        CHECK_EQ_INT(g->npending, 0);
        gateFree(g);
    }
}

/* ----------------------------- The drift detector ------------------------
 * gate_stats.h has the detector; these are the gate's part: which request
 * is a spot check, the number it gets and when, what is reported under
 * that number and when, and what the gate does with each verdict. */

/* Spot checks are numbered per label, in the order they are asked. */
TEST(drift_spot_numbers) {
    const uint32_t spot = GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_SERV;
    gate *g = trainedGate(12);
    gatePredictResult a[4], b[3], r;
    int64_t now = T0;
    gateEval ev;
    uint32_t i;

    for (i = 0; i < 39; i++) feedText(g, now, U_PLAIN, &XA, 0);
    for (i = 0; i < 39; i++) feedText(g, now, U_PLAIN, &XB, 1);
    CHECK(servingOf(g)->gen == 1 && servingOf(g)->policy[0] == 0 && servingOf(g)->policy[1] == 0);
    /* While label1 had no threshold its requests were proof, but not spot
     * checks of answers: nothing was numbered. */
    expectDrift(&servingOf(g)->drift[0], 0, 0, 0, 0);
    expectDrift(&servingOf(g)->drift[1], 0, 0, 0, 0);
    /* A number is handed out when a spot check is asked, each label
     * counting for itself; a request the gate answers gets none. */
    a[0] = askText(g, now, U_SPOT, &XA);
    b[0] = askText(g, now, U_SPOT, &XB);
    CHECK(askText(g, now, U_PLAIN, &XA).action == GATE_ACTION_ACCEPT);
    a[1] = askText(g, now, U_SPOT, &XA);
    a[2] = askText(g, now, U_SPOT, &XA);
    b[1] = askText(g, now, U_SPOT, &XB);
    CHECK(askText(g, now, U_PLAIN, &XB).action == GATE_ACTION_ACCEPT);
    a[3] = askText(g, now, U_SPOT, &XA);
    b[2] = askText(g, now, U_SPOT, &XB);
    for (i = 0; i < 4; i++) {
        CHECK_EQ_INT(flagsOf(g, a[i].seq) & spot, spot);
        CHECK_EQ_INT(spotOf(g, a[i].seq), i + 1);
    }
    for (i = 0; i < 3; i++) CHECK_EQ_INT(spotOf(g, b[i].seq), i + 1);
    /* Nor does a spot check of something the gate would not have answered
     * (a guess made up for the purpose: label0 at 0.4, under 0.5). */
    ev = guesses(g, 0, 0.4, 0, 0.4);
    r = askGuess(g, now, U_SPOT, &ev, &XA);
    CHECK(r.action == GATE_ACTION_ABSTAIN);
    CHECK_EQ_INT(flagsOf(g, r.seq) & spot, GATE_OBS_AUDIT | GATE_OBS_SERV);
    CHECK_EQ_INT(spotOf(g, r.seq), 0);
    expectDrift(&servingOf(g)->drift[0], 4, 0, 0, 0);
    expectDrift(&servingOf(g)->drift[1], 3, 0, 0, 0);
    /* An answer that disagrees is reported the moment it arrives, under
     * the number of its spot check, to the detector of its label: the
     * second of label1 first, then the fourth and the first of label0. */
    CHECK_EQ_INT(answer(g, now, b[1].seq, 0), 1);
    expectDrift(&servingOf(g)->drift[1], 3, 1, 1, 0);
    expectDrift(&servingOf(g)->drift[0], 4, 0, 0, 0);
    CHECK_EQ_INT(answer(g, now, a[3].seq, 1), 1);
    CHECK_EQ_INT(answer(g, now, a[0].seq, 1), 1);
    expectDrift(&servingOf(g)->drift[0], 4, 2, 2, 0);
    expectDrift(&servingOf(g)->drift[1], 3, 1, 1, 0);
    /* Answers that agree are not reported at all. */
    CHECK_EQ_INT(answer(g, now, a[1].seq, 0), 1);
    CHECK_EQ_INT(answer(g, now, b[0].seq, 1), 1);
    expectDrift(&servingOf(g)->drift[0], 4, 2, 2, 0);
    expectDrift(&servingOf(g)->drift[1], 3, 1, 1, 0);
    CHECK_EQ_INT(servingOf(g)->audits, 5);
    CHECK_EQ_INT(servingOf(g)->audit_errors, 3);
    /* stat(4, 2) = 2.25 and stat(3, 1) = 0.9375. */
    CHECK_NEAR(gateDriftLogStat(&servingOf(g)->drift[0], &g->drift_bets[0], 0), log(2.25), 1e-12);
    CHECK_NEAR(gateDriftLogStat(&servingOf(g)->drift[1], &g->drift_bets[1], 0), log(0.9375), 1e-12);
    /* A spot check that is given up is reported like that too, to the
     * detector of its label: the third of label0 and the third of label1
     * never get an answer. (Proof moves on without the first of the two
     * after the grace period. The made-up request is given up with them:
     * it was no spot check and is lost to the proof only.) */
    expectDue(g, now + GRACE);
    CHECK_EQ_INT(flagsOf(g, a[2].seq) & GATE_OBS_PROVISIONAL, GATE_OBS_PROVISIONAL);
    expectDue(g, now + TIMEOUT);
    CHECK_EQ_INT(g->npending, 0);
    CHECK_EQ_INT(g->stats.missing, 3);
    CHECK_EQ_INT(g->missing_run, 0); /* answers arrived after each was asked */
    expectDrift(&servingOf(g)->drift[0], 4, 2, 2, 1);
    expectDrift(&servingOf(g)->drift[1], 3, 1, 1, 1);
    /* With the lost one counted as a disagreement: stat(4, 3) = 9.75 and
     * stat(3, 2) = 3.75. */
    CHECK_NEAR(gateDriftLogStat(&servingOf(g)->drift[0], &g->drift_bets[0], 1), log(9.75), 1e-12);
    CHECK_NEAR(gateDriftLogStat(&servingOf(g)->drift[1], &g->drift_bets[1], 1), log(3.75), 1e-12);
    CHECK_EQ_INT(servingOf(g)->audits, 5);
    gateFree(g);
}

/* Twelve spot checks are asked; then their answers arrive, in one order or
 * another, the second half of them later, when proof has moved on without
 * what is still unanswered. Eight disagree: the block is at stat(12, 8) =
 * 245.5 in the end whatever the order was, and the alarm is never raised.
 * (A detector that follows the answers as they come and starts over when
 * it falls, like the one before, fires at seven disagreements in a row:
 * every order that brings seven of the eight before an agreement.) Nine
 * disagree: stat(12, 9) = 1338.2, and the alarm is raised by the ninth
 * disagreement to arrive, whichever that is. */
TEST(drift_order_and_batches) {
    enum { N = 12, ORDERS = 7 };
    static const int disagree[N] = {1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1}; /* nine */
    uint64_t rng = 20261128;
    int nine, order;

    CHECK_NEAR(exp(driftStat(ALPHA, 12, 8)), 245.53125, 1e-9);
    for (nine = 0; nine < 2; nine++) {
        for (order = 0; order < ORDERS; order++) {
            gate *g = activeScoreGate(T0);
            uint32_t idx[N], i, j, n = 0, wrong = 0;
            int64_t now = T0;
            uint64_t seq[N];

            for (i = 0; i < N; i++) {
                seq[i] = ask(g, T0, U_SPOT, 0.9).seq;
                CHECK_EQ_INT(spotOf(g, seq[i]), i + 1);
            }
            switch (order) {
            case 0: /* as they were asked */
                for (i = 0; i < N; i++) idx[i] = i;
                break;
            case 1: /* the newest first */
                for (i = 0; i < N; i++) idx[i] = N - 1 - i;
                break;
            case 2: /* every disagreement first */
            case 3: /* every agreement first */
                for (j = 0; j < 2; j++)
                    for (i = 0; i < N; i++)
                        if (disagree[i] == (order == 2 ? !j : (int)j)) idx[n++] = i;
                break;
            default: /* any order */
                for (i = 0; i < N; i++) idx[i] = i;
                for (i = N - 1; i > 0; i--) {
                    uint32_t k = (uint32_t)(gate_test_rand64(&rng) % (i + 1)), tmp = idx[i];

                    idx[i] = idx[k];
                    idx[k] = tmp;
                }
                break;
            }
            for (i = 0; i < N; i++) {
                uint32_t k = idx[i];
                /* With eight, the last spot check agrees. */
                int wrong_one = disagree[k] && (nine || k != N - 1);

                if (i == N / 2) now = T0 + GRACE + 10; /* the second batch */
                CHECK_EQ_INT(gateLearn(g, now, EPOCH, seq[k], !wrong_one), 1);
                inv(g);
                wrong += (uint32_t)wrong_one;
                CHECK_EQ_INT(g->serving == NULL, wrong == 9);
                if (g->serving == NULL) break;
                expectDrift(&servingOf(g)->drift[0], N, wrong, wrong, 0);
            }
            CHECK_EQ_INT(g->stats.revocations, nine);
            if (nine) {
                CHECK(g->candidate != NULL && candidateOf(g)->gen == 2);
                CHECK_EQ_INT(g->blind, 0);
            } else {
                CHECK_NEAR(gateDriftLogStat(&servingOf(g)->drift[0], &g->drift_bets[0], 0),
                           log(245.53125), 1e-12);
                CHECK(servingOf(g)->audits == N && servingOf(g)->audit_errors == 8);
                /* The proof holds the twelve as well, whenever it got them. */
                CHECK(servingOf(g)->samples == 35 + N && servingOf(g)->errors == 8);
                CHECK_EQ_INT(g->npending, 0);
            }
            gateFree(g);
        }
    }
}

/* A disagreement that arrives after the block of 64 its spot check was
 * asked in has ended counts in the blocks of 512 and of 4096, which are
 * still open, and not in the block of 64 that is open now. And a spot
 * check that proof has moved on without is, to the detector, a spot check
 * without an answer like any other. */
TEST(drift_block_ended) {
    const uint32_t spot = GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_SERV;
    int64_t t = T0;
    gate *g = activeScoreGate(t);
    gatePredictResult first[3], r;
    const gateDrift *d;
    uint32_t i;

    /* Spot checks 1, 2 and 3 get no answer for now. 64 more are asked and
     * answered right: number 67 is the third of the second block of 64. */
    for (i = 0; i < 3; i++) first[i] = ask(g, t, U_SPOT, 0.9);
    for (i = 4; i <= 67; i++) {
        r = ask(g, t, U_SPOT, 0.9);
        CHECK_EQ_INT(spotOf(g, r.seq), i);
        CHECK_EQ_INT(answer(g, t, r.seq, 1), 1);
    }
    d = &servingOf(g)->drift[0];
    CHECK(d->asked == 67 && d->errors == 0);
    CHECK(gateDriftOpen(d, 0) == 3 && gateDriftOpen(d, 1) == 67 && gateDriftOpen(d, 2) == 67);
    /* Proof moves on without the three: wrong guesses to the proof,
     * nothing to the detector. */
    expectDue(g, t + GRACE);
    for (i = 0; i < 3; i++) CHECK_EQ_INT(flagsOf(g, first[i].seq), spot | GATE_OBS_PROVISIONAL);
    CHECK(servingOf(g)->samples == 35 + 67 && servingOf(g)->errors == 3);
    CHECK(d->errors == 0 && d->wrong[0] + d->wrong[1] + d->wrong[2] == 0 &&
          d->lost[0] + d->lost[1] + d->lost[2] == 0);
    /* The answer of number 1 arrives and disagrees. Its block of 64 has
     * ended: it is counted in the two longer ones only. */
    CHECK_EQ_INT(answer(g, t + GRACE, first[0].seq, 0), 1);
    CHECK(g->serving != NULL);
    CHECK(d->asked == 67 && d->errors == 1);
    CHECK(d->wrong[0] == 0 && d->wrong[1] == 1 && d->wrong[2] == 1);
    CHECK_EQ_INT(servingOf(g)->errors, 3); /* what the proof had counted it as */
    CHECK_EQ_INT(servingOf(g)->audit_errors, 1);
    /* The answer of number 2 agrees: the proof takes its wrong guess back,
     * the detector hears nothing. */
    CHECK_EQ_INT(answer(g, t + GRACE, first[1].seq, 1), 1);
    CHECK_EQ_INT(servingOf(g)->errors, 2);
    CHECK(d->errors == 1 && d->wrong[0] == 0 && d->wrong[1] == 1 && d->wrong[2] == 1);
    CHECK_EQ_INT(servingOf(g)->audits, 64 + 2);
    /* The open block of 64 holds three spot checks that agreed (65, 66 and
     * 67): with those it takes nine disagreements (stat(12, 9) = 1338.2;
     * stat(11, 8) = 464.1). The late one is not among them: eight more do
     * not raise the alarm, the ninth does. (Had it been counted in this
     * block, the eighth would have: stat(11, 9) = 2604.4.) The block of
     * 512 is nowhere near: 2^10 0.75^66 is 6e-6. */
    for (i = 1; i <= 9; i++) {
        r = ask(g, t + GRACE, U_SPOT, 0.9);
        CHECK_EQ_INT(spotOf(g, r.seq), 67 + i);
        CHECK_EQ_INT(answer(g, t + GRACE, r.seq, 0), 1);
        CHECK_EQ_INT(g->serving != NULL, i < 9);
        if (g->serving == NULL) break;
        CHECK(d->wrong[0] == i && d->wrong[1] == i + 1 && d->wrong[2] == i + 1);
        CHECK_EQ_INT(gateDriftOpen(d, 0), 3 + i);
        CHECK_NEAR(gateDriftLogStat(d, &g->drift_bets[0], 0), driftStat(ALPHA, 3 + i, i), 1e-12);
    }
    CHECK_EQ_INT(i, 9);
    CHECK_EQ_INT(g->stats.revocations, 1);
    /* Number 3 never had an answer. Proof had moved on without it, so it
     * still has its id; its answer is used and changes nothing. */
    CHECK_EQ_INT(g->npending, 1);
    CHECK_EQ_INT(answer(g, t + GRACE, first[2].seq, 0), 1);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 2 && candidateOf(g)->samples == 0);
    CHECK_EQ_INT(g->npending, 0);
    gateFree(g);
}

/* The blocks themselves. A block of 64 begins with the spot checks number
 * 1, 65, 129, ..., a block of 512 with 1, 513, ..., a block of 4096 with 1
 * and 4097. What a block has counted is forgotten when the next block of
 * its length begins, not a spot check sooner, and the longer blocks keep
 * what they have.
 *
 * Then three sequences of answers, each of which only one of the three
 * lengths sees (exact arithmetic, by the rule of SPEC.md section 5):
 *   - 30 spot checks agree, the following disagree: the 24th of those
 *     stops the gate (spot check 54: stat(54, 24) = 1629.6, stat(53, 23) =
 *     792.9). With blocks of 32 it would be the 9th: a fresh block begins
 *     at number 33.
 *   - 256 agree, then two of every five disagree ("x.x.."): no block of 64
 *     gets to 1000, and the block of 512 that holds the 256 never will. The
 *     one that begins at 513 does, at number 584 (72 asked, 29 of them
 *     wrong: 1142.8). Blocks of 256 would be there at 324, and without the
 *     512 it takes until 1027.
 *   - 30 of every 101 disagree, evenly spread: too few for any block of 64
 *     or 512, which end before they amount to anything. The block of 4096
 *     is at 1003.0 with number 2084 (619 wrong). */
TEST(drift_blocks) {
    static char pattern[4097];
    gatePredictResult lost[3], r;
    const gateDrift *d;
    int64_t t = T0;
    gate *g = activeScoreGate(t);
    uint32_t i, n = 0;

    /* Spot checks 1 to 64: number 5 and number 9 disagree, 12, 20 and 33
     * get no answer, the others agree. */
    for (i = 1; i <= 64; i++) {
        r = ask(g, t, U_SPOT, 0.9);
        CHECK_EQ_INT(spotOf(g, r.seq), i);
        if (i == 12 || i == 20 || i == 33) lost[n++] = r;
        else CHECK_EQ_INT(answer(g, t, r.seq, !(i == 5 || i == 9)), 1);
    }
    expectDue(g, t + GRACE);
    for (i = 0; i < 3; i++)
        CHECK_EQ_INT(flagsOf(g, lost[i].seq) & GATE_OBS_PROVISIONAL, GATE_OBS_PROVISIONAL);
    expectDue(g, t + TIMEOUT);
    t += TIMEOUT;
    CHECK(g->serving != NULL && g->npending == 0);
    CHECK_EQ_INT(g->stats.missing, 3);
    CHECK_EQ_INT(g->missing_run, 0);
    d = &servingOf(g)->drift[0];
    expectDrift(d, 64, 2, 2, 3);
    CHECK(servingOf(g)->samples == 35 + 64 && servingOf(g)->errors == 5);
    /* Number 65 opens the second block of 64. */
    feed(g, t, U_SPOT, 0.9, 1);
    CHECK(d->asked == 65 && d->errors == 2);
    CHECK(d->wrong[0] == 0 && d->lost[0] == 0 && gateDriftOpen(d, 0) == 1);
    CHECK(d->wrong[1] == 2 && d->lost[1] == 3 && gateDriftOpen(d, 1) == 65);
    CHECK(d->wrong[2] == 2 && d->lost[2] == 3 && gateDriftOpen(d, 2) == 65);
    CHECK_NEAR(gateDriftLogStat(d, &g->drift_bets[0], 0), log(0.625), 1e-12); /* stat(1, 0) */
    /* 512 closes the first block of 512, 513 opens the second. */
    for (i = 66; i <= 512; i++) feed(g, t, U_SPOT, 0.9, 1);
    CHECK(d->asked == 512 && gateDriftOpen(d, 0) == 64);
    CHECK(d->wrong[1] == 2 && d->lost[1] == 3 && gateDriftOpen(d, 1) == 512);
    feed(g, t, U_SPOT, 0.9, 1);
    CHECK(d->wrong[0] == 0 && d->lost[0] == 0 && gateDriftOpen(d, 0) == 1);
    CHECK(d->wrong[1] == 0 && d->lost[1] == 0 && gateDriftOpen(d, 1) == 1);
    CHECK(d->wrong[2] == 2 && d->lost[2] == 3 && gateDriftOpen(d, 2) == 513);
    /* And 4097 the second block of 4096. */
    for (i = 514; i <= 4096; i++) feed(g, t, U_SPOT, 0.9, 1);
    CHECK(d->asked == 4096 && gateDriftOpen(d, 0) == 64 && gateDriftOpen(d, 1) == 512);
    CHECK(d->wrong[2] == 2 && d->lost[2] == 3 && gateDriftOpen(d, 2) == 4096);
    feed(g, t, U_SPOT, 0.9, 1);
    CHECK(d->asked == 4097 && d->errors == 2);
    for (i = 0; i < GATE_DRIFT_SCALES; i++)
        CHECK(d->wrong[i] == 0 && d->lost[i] == 0 && gateDriftOpen(d, (int)i) == 1);
    CHECK(g->serving != NULL && servingOf(g)->audits == 4097 - 3);
    CHECK_EQ_INT(servingOf(g)->audit_errors, 2);
    gateFree(g);

    /* 30 spot checks that agree, then disagreements: the 24th. */
    t = T0;
    g = activeScoreGate(t);
    for (i = 0; i < 30; i++) feed(g, t, U_SPOT, 0.9, 1);
    for (i = 1; i <= 24; i++) {
        r = ask(g, t, U_SPOT, 0.9);
        CHECK_EQ_INT(spotOf(g, r.seq), 30 + i);
        CHECK_EQ_INT(answer(g, t, r.seq, 0), 1);
        CHECK_EQ_INT(g->serving != NULL, i < 24);
        if (g->serving == NULL) break;
        CHECK_NEAR(gateDriftLogStat(&servingOf(g)->drift[0], &g->drift_bets[0], 0),
                   driftStat(ALPHA, 30 + i, i), 1e-12);
    }
    CHECK_EQ_INT(i, 24);
    CHECK_EQ_INT(g->stats.revocations, 1);
    CHECK(driftStat(ALPHA, 54, 24) >= log(1000.0) && driftStat(ALPHA, 53, 23) < log(1000.0));
    gateFree(g);

    /* The three sequences, to the detector by itself. */
    memset(pattern, '.', 30);
    memset(pattern + 30, 'x', 60);
    pattern[90] = '\0';
    CHECK_EQ_INT(alarmAfter(ALPHA, pattern), 54);
    memset(pattern, '.', 256);
    for (i = 256; i < 1256; i++) pattern[i] = "x.x.."[(i - 256) % 5];
    pattern[1256] = '\0';
    CHECK_EQ_INT(alarmAfter(ALPHA, pattern), 584);
    for (i = 1; i <= 4096; i++)
        pattern[i - 1] = (i * 30) / 101 != ((i - 1) * 30) / 101 ? 'x' : '.';
    pattern[4096] = '\0';
    CHECK_EQ_INT(alarmAfter(ALPHA, pattern), 2084);
    CHECK(driftStat(ALPHA, 2084, 619) >= log(1000.0) && driftStat(ALPHA, 2084, 618) < log(1000.0));
    CHECK(driftStat(ALPHA, 72, 29) >= log(1000.0) && driftStat(ALPHA, 72, 28) < log(1000.0));
}

/* A spot check whose answer is given up is not a disagreement, but it may
 * hide one. The detector counts lost answers next to the disagreements,
 * and when the two together would mean "worse" at the level 1e9 the gate
 * can no longer be checked: it stops like a gate whose answers have
 * stopped (both snapshots dropped, the model kept, no snapshot until an
 * answer arrives). */
TEST(drift_lost_spot_checks) {
    int64_t t = T0;
    gate *g = trainedGate(12);
    gatePredictResult r;
    uint64_t weights;
    uint32_t i;

    /* A classifier that answers label0, with a second snapshot under test. */
    for (i = 0; i < 39; i++) feedText(g, t, U_PLAIN, &XA, 0);
    CHECK(servingOf(g)->gen == 1 && candidateOf(g)->gen == 2);
    CHECK_EQ_INT(g->trainer.updates, 51);
    weights = mixModel(0, &g->trainer, 0);
    /* 19 spot checks of its answers that nobody answers: lost. For "cannot
     * be checked" the block is at stat(19, 19) = 5.8e8, under 1e9. For
     * "worse" it is at 0.75^19 or so: nobody disagreed. The gate goes on
     * answering. */
    for (i = 0; i < 19; i++) CHECK(askText(g, t, U_SPOT, &XA).action == GATE_ACTION_AUDIT);
    expectDue(g, t + TIMEOUT);
    t += TIMEOUT;
    CHECK(g->serving != NULL && g->candidate != NULL && g->blind == 0);
    CHECK_EQ_INT(g->stats.missing, 19);
    CHECK_EQ_INT(g->missing_run, 19);
    expectDrift(&servingOf(g)->drift[0], 19, 0, 0, 19);
    CHECK_NEAR(gateDriftLogStat(&servingOf(g)->drift[0], &g->drift_bets[0], 1), log(581392877.5),
               1e-12);
    CHECK(gateDriftLogStat(&servingOf(g)->drift[0], &g->drift_bets[0], 0) < log(0.01));
    CHECK_EQ_INT(servingOf(g)->audits, 0);
    CHECK(servingOf(g)->samples == 39 + 19 && servingOf(g)->errors == 19);
    CHECK(askText(g, t, U_PLAIN, &XA).action == GATE_ACTION_ACCEPT);
    /* The twentieth: stat(20, 20) = 1.74e9. The gate cannot rule out any
     * more that it got worse, and stops: both snapshots go. Lost answers
     * say nothing about the model: it stays, bit for bit, and is frozen
     * again when an answer arrives. (It is not the run of missing answers
     * that stopped the gate: that is at 20 of 32.) */
    CHECK(askText(g, t, U_SPOT, &XA).action == GATE_ACTION_AUDIT);
    expectDue(g, t + TIMEOUT);
    t += TIMEOUT;
    CHECK_EQ_INT(g->blind, 1);
    CHECK(g->serving == NULL && g->candidate == NULL);
    CHECK(gateState(g) == GATE_STATE_COLD);
    CHECK_EQ_INT(g->stats.revocations, 1);
    CHECK_EQ_INT(g->stats.missing, 20);
    CHECK_EQ_INT(g->missing_run, 0);
    CHECK_EQ_INT(g->generations, 2);
    CHECK_EQ_INT(g->trainer.updates, 51);
    CHECK(mixModel(0, &g->trainer, 0) == weights);
    CHECK_EQ_INT(g->snapshot_updates, 0);
    expectIdle(g, t + 5);
    r = askText(g, t + 5, U_PLAIN, &XA);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0);
    CHECK_EQ_INT(answer(g, t + 5, r.seq, 0), 1);
    CHECK(g->blind == 0 && g->candidate != NULL && candidateOf(g)->gen == 3);
    CHECK_EQ_INT(candidateOf(g)->model.updates, 52);
    gateFree(g);

    /* After spot checks that agreed it takes more lost ones: three that
     * agreed and 22 lost ones (stat(25, 22) = 1.96e9; 21 give 3.3e8). */
    CHECK(driftStat(ALPHA, 25, 22) >= log(1e9) && driftStat(ALPHA, 25, 21) < log(1e9));
    t = T0;
    g = activeScoreGate(t);
    for (i = 0; i < 3; i++) feed(g, t, U_SPOT, 0.9, 1);
    for (i = 0; i < 21; i++) CHECK(ask(g, t, U_SPOT, 0.9).action == GATE_ACTION_AUDIT);
    CHECK(ask(g, t + 1, U_SPOT, 0.9).action == GATE_ACTION_AUDIT);
    expectDue(g, t + TIMEOUT);
    CHECK(g->serving != NULL && g->blind == 0);
    expectDrift(&servingOf(g)->drift[0], 25, 0, 0, 21);
    CHECK_EQ_INT(g->missing_run, 21);
    expectDue(g, t + TIMEOUT + 1);
    CHECK(g->serving == NULL && g->candidate == NULL && g->blind == 1);
    CHECK_EQ_INT(g->stats.revocations, 1);
    CHECK_EQ_INT(g->stats.missing, 22);
    CHECK_EQ_INT(g->generations, 1); /* a blind score gate does not start its next test */
    gateFree(g);

    /* Lost answers and disagreements together. Ten lost spot checks
     * (stat(10, 10) = 30036.5), then wrong ones. For "worse" the lost ones
     * count as right: after nine disagreements the block is at stat(19, 9)
     * = 24.0, after ten at 57.7, far from 1000. For "cannot be checked"
     * they count like the disagreements: the tenth makes it stat(20, 20) =
     * 1.74e9. That verdict is raised by an answer, and it is not "worse":
     * what the model has learned stays (61 examples: the alarm that says
     * "worse" would have left none), no snapshot is under test, and the
     * next answer lets the gate start again. */
    t = T0;
    g = trainedGate(12);
    for (i = 0; i < 39; i++) feedText(g, t, U_PLAIN, &XA, 0);
    for (i = 0; i < 10; i++) CHECK(askText(g, t, U_SPOT, &XA).action == GATE_ACTION_AUDIT);
    expectDue(g, t + TIMEOUT);
    t += TIMEOUT;
    expectDrift(&servingOf(g)->drift[0], 10, 0, 0, 10);
    CHECK_NEAR(gateDriftLogStat(&servingOf(g)->drift[0], &g->drift_bets[0], 1), log(30036.5), 1e-12);
    for (i = 1; i <= 10; i++) {
        r = askText(g, t, U_SPOT, &XA);
        CHECK(r.action == GATE_ACTION_AUDIT);
        CHECK_EQ_INT(answer(g, t, r.seq, 1), 1);
        CHECK_EQ_INT(g->serving != NULL, i < 10);
        if (g->serving == NULL) break;
        expectDrift(&servingOf(g)->drift[0], 10 + i, i, i, 10);
        CHECK_NEAR(gateDriftLogStat(&servingOf(g)->drift[0], &g->drift_bets[0], 0),
                   driftStat(ALPHA, 10 + i, i), 1e-12);
    }
    CHECK_EQ_INT(i, 10);
    CHECK_NEAR(exp(driftStat(ALPHA, 19, 9)), 98415.0 / 4096.0, 1e-9);
    CHECK_EQ_INT(g->stats.revocations, 1);
    CHECK(g->serving == NULL && g->candidate == NULL);
    CHECK_EQ_INT(g->blind, 1);
    CHECK_EQ_INT(g->missing_run, 0);
    CHECK_EQ_INT(g->generations, 2);
    CHECK_EQ_INT(g->trainer.updates, 51 + 10);
    CHECK_EQ_INT(g->snapshot_updates, 0);
    feedText(g, t, U_PLAIN, &XA, 0);
    CHECK(g->blind == 0 && g->candidate != NULL && candidateOf(g)->gen == 3);
    CHECK_EQ_INT(candidateOf(g)->model.updates, 62);
    gateFree(g);
}

/* A paused gate answers nothing and keeps checking itself: what it would
 * have answered is spot checked, numbered and counted as before, and both
 * alarms stop it for when it is resumed. */
TEST(drift_paused_gate) {
    int64_t t = T0;
    gate *g = activeScoreGate(t);
    gatePredictResult r;
    uint32_t i;

    gatePause(g, t);
    /* What would be an ACCEPT: nothing is recorded, nothing numbered. */
    r = ask(g, t, U_PLAIN, 0.9);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq == 0);
    for (i = 1; i <= 7; i++) {
        r = ask(g, t, U_SPOT, 0.9);
        CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0);
        CHECK_EQ_INT(spotOf(g, r.seq), i);
        CHECK_EQ_INT(answer(g, t, r.seq, 0), 1);
        CHECK_EQ_INT(g->serving != NULL, i < 7);
        if (g->serving) expectDrift(&servingOf(g)->drift[0], i, i, i, 0);
    }
    CHECK(gateState(g) == GATE_STATE_PAUSED);
    CHECK_EQ_INT(g->stats.revocations, 1);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 2);
    CHECK(g->stats.audited == 0 && g->stats.accepted == 0);
    gateResume(g, t);
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    gateFree(g);

    /* Lost spot checks of a paused gate: the twentieth. */
    g = activeScoreGate(t);
    gatePause(g, t);
    (void)loseAnswers(g, t, 20, U_SPOT);
    CHECK(g->blind == 1 && g->serving == NULL && g->candidate == NULL);
    CHECK(gateState(g) == GATE_STATE_PAUSED);
    CHECK_EQ_INT(g->stats.missing, 20);
    CHECK_EQ_INT(g->stats.revocations, 1);
    gateResume(g, t + TIMEOUT);
    CHECK(gateState(g) == GATE_STATE_COLD);
    gateFree(g);
}

/* The number of a spot check belongs to the detector that handed it out:
 * to one label of one generation. When that generation is replaced while
 * spot checks are outstanding, their answers, and their loss, are nothing
 * to the detectors of the generation that serves now, which hands out the
 * same numbers again. */
TEST(drift_generation_replaced) {
    const uint32_t both = GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_SERV | GATE_OBS_CAND;
    int64_t t = T0;
    gate *g = twoGenerations(t);
    gatePredictResult old[4], r;
    gateEval ev;
    uint32_t i;

    /* Four spot checks of answers of generation 1 (label0 at 0.99) that
     * get no answer for now: the numbers 1 to 4 of its label0. Generation
     * 2 says label1 for them. */
    for (i = 0; i < 4; i++) {
        ev = guesses(g, 0, 0.99, 1, 0.6);
        old[i] = askGuess(g, t, U_SPOT, &ev, &XA);
        CHECK_EQ_INT(flagsOf(g, old[i].seq), both);
        CHECK_EQ_INT(spotOf(g, old[i].seq), i + 1);
    }
    /* 44 spot checks that only generation 2 would answer, all right: they
     * wait behind the four. */
    for (i = 0; i < 44; i++) {
        ev = guesses(g, 1, 0.8, 0, 0.99);
        r = askGuess(g, t, U_SPOT, &ev, &XA);
        CHECK_EQ_INT(spotOf(g, r.seq), 0);
        CHECK_EQ_INT(answer(g, t, r.seq, 0), 1);
    }
    CHECK_EQ_INT(candidateOf(g)->samples, 0);
    expectDrift(&servingOf(g)->drift[0], 4, 0, 0, 0);
    /* Proof moves on without the four, the 44 are applied, and generation
     * 2 takes over: it answers 44 of its requests, generation 1 four of
     * them. Generation 3 follows under test. */
    expectDue(g, t + GRACE);
    CHECK(servingOf(g)->gen == 2 && g->candidate != NULL && candidateOf(g)->gen == 3);
    CHECK_EQ_INT(servingOf(g)->samples, 48);
    CHECK_EQ_INT(servingOf(g)->errors, 4);
    for (i = 0; i < 4; i++) {
        CHECK_EQ_INT(flagsOf(g, old[i].seq), both | GATE_OBS_PROVISIONAL);
        CHECK_EQ_INT(spotOf(g, old[i].seq), i + 1);
    }
    /* The detectors of generation 2 start from nothing. */
    expectDrift(&servingOf(g)->drift[0], 0, 0, 0, 0);
    expectDrift(&servingOf(g)->drift[1], 0, 0, 0, 0);
    /* Generation 2 has spot checks of its own: five, all right. The
     * numbers 1 to 5 of its label0. */
    for (i = 1; i <= 5; i++) {
        ev = guesses(g, 0, 0.99, 0, 0.99);
        r = askGuess(g, t + GRACE, U_SPOT, &ev, &XA);
        CHECK(r.action == GATE_ACTION_AUDIT);
        CHECK_EQ_INT(spotOf(g, r.seq), i);
        CHECK_EQ_INT(answer(g, t + GRACE, r.seq, 0), 1);
    }
    expectDrift(&servingOf(g)->drift[0], 5, 0, 0, 0);
    CHECK_EQ_INT(servingOf(g)->audits, 5);
    /* Now the answers of three of the old spot checks arrive, and they
     * disagree with what generation 1 said: label1. They carry the numbers
     * 1, 2 and 3 of a detector that is gone: nothing to the one that
     * generation 2 runs under the same numbers. To generation 2 they are
     * proof that it guessed right (it said label1): it takes three wrong
     * guesses back. */
    for (i = 0; i < 3; i++) CHECK_EQ_INT(answer(g, t + GRACE + 1, old[i].seq, 1), 1);
    expectDrift(&servingOf(g)->drift[0], 5, 0, 0, 0);
    expectDrift(&servingOf(g)->drift[1], 0, 0, 0, 0);
    CHECK(servingOf(g)->audits == 5 && servingOf(g)->audit_errors == 0);
    CHECK_EQ_INT(servingOf(g)->errors, 1);
    CHECK(g->serving != NULL && g->stats.revocations == 0);
    /* The fourth never gets one: a missing answer for the proof of
     * generation 2, not a lost spot check of it. */
    expectDue(g, t + TIMEOUT);
    CHECK_EQ_INT(g->stats.missing, 1);
    CHECK_EQ_INT(g->missing_run, 0);
    expectDrift(&servingOf(g)->drift[0], 5, 0, 0, 0);
    CHECK(servingOf(g)->gen == 2 && servingOf(g)->errors == 1);
    gateFree(g);
}

/* ROBUSTNESS. A score gate has no labels and keeps no clues: a guess that
 * carries a label is refused, and clues that come with a request stay with
 * the caller. */
TEST(score_gate_refuses_labels_and_clues) {
    gatePredictResult r;
    const gateObs *o;
    gateEval ev, bad;
    uint64_t before;
    gateSparse x;
    int64_t t;
    uint32_t i;
    int kind;
    gate *g;

    /* A gate under test, one that answers, one that is blind. */
    for (kind = 0; kind < 3; kind++) {
        t = T0;
        g = kind == 1 ? activeScoreGate(t) : scoreGate();
        if (kind == 2) {
            t = loseAnswers(g, t, 32, U_PLAIN);
            CHECK_EQ_INT(g->blind, 1);
        }
        gateMaintain(g, t);
        CHECK(gateEvaluate(g, NULL, 0.9, &ev) == GATE_OK);
        CHECK(ev.serving.label == -1 && ev.candidate.label == -1);
        before = fingerprint(g, 0);
        /* A label on either guess, whether there is such a generation or
         * not, whatever the draw. */
        for (i = 0; i < 6; i++) {
            static const int32_t labels[3] = {0, 63, -2};

            bad = ev;
            if (i & 1) bad.serving.label = labels[i / 2]; else bad.candidate.label = labels[i / 2];
            CHECK(gatePredict(g, t, U_SPOT, &bad, NULL, &r) == GATE_ERR);
            CHECK(r.seq == 0);
            CHECK(gatePredict(g, t, U_PLAIN, &bad, NULL, &r) == GATE_ERR);
            CHECK_EQ_INT(gatePredictNeedsFeatures(g, U_SPOT, &bad), 0);
        }
        CHECK(fingerprint(g, 0) == before);
        inv(g);
        /* Clues handed in with a request are not taken: the request is
         * recorded without them. */
        CHECK_EQ_INT(gatePredictNeedsFeatures(g, U_SPOT, &ev), 0);
        if (gateSparseCopy(&x, &XA) != GATE_OK) die("out of memory");
        CHECK(gatePredict(g, t, U_SPOT, &ev, &x, &r) == GATE_OK);
        CHECK(r.seq != 0);
        CHECK(x.n == 1 && x.idx != NULL && x.val != NULL);
        o = gateFindObservation(g, r.seq);
        CHECK(o != NULL && o->x.n == 0 && o->x.idx == NULL && o->x.val == NULL);
        CHECK_EQ_INT(g->pending_bytes, GATE_OBS_BYTES(0));
        CHECK_EQ_INT(gateMemoryUsage(g), expectBytes(g));
        inv(g);
        gateSparseFree(&x);
        CHECK_EQ_INT(answer(g, t, r.seq, 1), 1);
        gateFree(g);
    }
}

/* A score that is not a number. The module checks the range of every score
 * before the core sees it and the header says nothing about one that is
 * not finite, so this states the cautious reading only: the request is
 * either refused, and then nothing has changed, or it is a request that
 * lies above no threshold. It is never answered, not even by a gate that
 * answers everything, and its answer is proof for no threshold. (The
 * encoding has no place for such a number: a gate that has recorded one
 * cannot be decoded again until the request is gone. That is why this test
 * checks the invariants itself and leaves the round trip of the scratch
 * build out. See the report.) */
TEST(score_that_is_not_a_number) {
    static const double bad[] = {NAN, -INFINITY}, us[] = {U_SPOT, U_PLAIN};
    uint64_t before, seen[GATE_THRESHOLDS];
    const gateGeneration *gen;
    gatePredictResult r;
    char err[160];
    gateEval ev;
    uint32_t t;
    int kind, i, j, rc;
    gate *g;

    for (kind = 0; kind < 2; kind++) {
        for (i = 0; i < 2; i++) {
            for (j = 0; j < 2; j++) {
                g = kind ? activeScoreGate(T0) : scoreGate();
                gen = kind ? servingOf(g) : candidateOf(g);
                for (t = 0; t < T; t++) seen[t] = gen->regions[t].n;
                gateMaintain(g, T0);
                before = fingerprint(g, 0);
                r.action = GATE_ACTION_ABSTAIN;
                r.seq = 0;
                rc = gateEvaluateFor(g, NULL, bad[i], us[j], &ev);
                if (rc == GATE_OK) rc = gatePredict(g, T0, us[j], &ev, NULL, &r);
                if (rc != GATE_OK) {
                    CHECK(fingerprint(g, 0) == before);
                } else {
                    CHECK_EQ_INT(r.action, GATE_ACTION_ABSTAIN);
                    CHECK_EQ_INT(r.label, -1);
                    if (r.seq != 0) {
                        CHECK(spotOf(g, r.seq) == 0); /* no spot check of an answer */
                        CHECK_EQ_INT(gateLearn(g, T0, g->epoch, r.seq, 1), 1);
                    }
                    gen = kind ? servingOf(g) : candidateOf(g);
                    CHECK(gen != NULL && gen->gen == 1);
                    for (t = 0; t < T; t++) CHECK_EQ_INT(gen->regions[t].n, seen[t]);
                }
                err[0] = '\0';
                CHECK(gateCheckInvariants(g, err, sizeof(err)));
                CHECK_EQ_STR(err, "");
                gateFree(g);
            }
        }
    }
}

/* gateCheckInvariants() knows the fields of the final round: a gate in
 * which one of them is out of line is refused, with a reason. */
TEST(invariants_of_the_new_fields) {
    int64_t t = T0;
    gate *g = twoGenerations(t);
    gatePredictResult r;
    char err[160];
    gateEval ev;
    uint32_t i;

#define OUT_OF_LINE(change, back) do { \
    change; \
    err[0] = '\0'; \
    CHECK(!gateCheckInvariants(g, err, sizeof(err))); \
    CHECK(err[0] != '\0'); \
    back; \
    CHECK(gateCheckInvariants(g, err, sizeof(err))); \
} while (0)

    /* Three proof requests of the candidate that generation 1 put at
     * label0, 0.99: the thresholds 0..19 hold three. */
    for (i = 0; i < 3; i++) {
        ev = guesses(g, 0, 0.99, 0, 0.99);
        r = askGuess(g, t, U_SPOT, &ev, &XA);
        CHECK_EQ_INT(answer(g, t, r.seq, 0), 1);
    }
    CHECK(candidateOf(g)->samples == 3 && candidateOf(g)->paired[19] == 3);
    OUT_OF_LINE(g->answer_mark = g->next_seq + 1, g->answer_mark = g->next_seq);
    OUT_OF_LINE(g->missing_run = GATE_MISSING_RUN, g->missing_run = 0);
    g->missing_run = GATE_MISSING_RUN - 1; /* one short of the alarm is a run like any other */
    CHECK(gateCheckInvariants(g, err, sizeof(err)));
    g->missing_run = 0;
    /* More requests than it was tested on; */
    OUT_OF_LINE(g->candidate->paired[0] = 4, g->candidate->paired[0] = 3);
    /* not nested: more at a higher threshold than at the one below it, */
    OUT_OF_LINE(g->candidate->paired[10] = 2, g->candidate->paired[10] = 3);
    /* ... in whichever row. */
    OUT_OF_LINE(g->candidate->paired[1 * T + 31] = 1, g->candidate->paired[1 * T + 31] = 0);
    /* Fewer at a higher threshold is what the counts are for. */
    g->candidate->paired[19] = 1;
    CHECK(gateCheckInvariants(g, err, sizeof(err)));
    g->candidate->paired[19] = 3;
    OUT_OF_LINE(g->serving->paired[0] = servingOf(g)->samples + 1, g->serving->paired[0] = 0);
    OUT_OF_LINE(g->blind = 1, g->blind = 0);
#undef OUT_OF_LINE
    inv(g);
    gateFree(g);
}

/* ---------------------------------- Full --------------------------------- */

TEST(full_gate) {
    static const double us[] = {0.0, U_SPOT, U_PLAIN}, scores[] = {0.0, 0.9, 1.0};
    int64_t now = T0;
    gate *g = activeScoreGate(now);
    gatePredictResult r;
    gateCounters st;
    uint64_t before, first = 0;
    uint32_t i, j;

    for (i = 0; i < GATE_MAX_PENDING; i++) {
        CHECK(!gateFull(g));
        r = ask(g, now, U_SPOT, 0.9);
        CHECK(r.action == GATE_ACTION_AUDIT && r.seq != 0);
        if (i == 0) first = r.seq;
    }
    CHECK(gateFull(g));
    CHECK_EQ_INT(g->npending, GATE_MAX_PENDING);

    /* Full: the gate does not look at the request. Not at a score it
     * would have answered, not at a draw it would have spot checked. */
    before = fingerprint(g, SKIP_REQUESTS);
    st = g->stats;
    for (i = 0; i < COUNTOF(us); i++) {
        for (j = 0; j < COUNTOF(scores); j++) {
            r = ask(g, now, us[i], scores[j]);
            CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq == 0 && r.label == -1);
        }
    }
    CHECK(fingerprint(g, SKIP_REQUESTS) == before);
    CHECK_EQ_INT(g->stats.requests, st.requests + 9);
    CHECK_EQ_INT(g->stats.abstained, st.abstained + 9);
    CHECK_EQ_INT(g->stats.full, 9);
    CHECK_EQ_INT(g->stats.accepted, st.accepted);
    CHECK(g->stats.window == st.window && g->stats.coverage == st.coverage);

    /* One answer makes room and the gate is itself again. */
    CHECK_EQ_INT(answer(g, now, first, 1), 1);
    CHECK(!gateFull(g));
    CHECK(ask(g, now, U_PLAIN, 0.9).action == GATE_ACTION_ACCEPT);
    CHECK(ask(g, now, U_SPOT, 0.9).action == GATE_ACTION_AUDIT);
    CHECK(gateFull(g));
    gateFree(g);
}

TEST(training_only_limit) {
    gate *g = classGate(2, 4);
    gatePredictResult r;
    int64_t now = T0;
    gateEval ev;
    uint32_t i;

    /* COLD: requests wait for a label to train on, up to 3072 of them. */
    for (i = 0; i < GATE_TRAIN_PENDING; i++) {
        r = askText(g, now, U_PLAIN, &XA);
        CHECK(r.seq != 0);
    }
    CHECK_EQ_INT(g->npending, GATE_TRAIN_PENDING);
    CHECK(!gateFull(g));
    gateMaintain(g, now);
    CHECK(gateEvaluate(g, &XA, 0.0, &ev) == GATE_OK);
    CHECK_EQ_INT(gatePredictNeedsFeatures(g, U_PLAIN, &ev), 0);
    r = askText(g, now, U_PLAIN, &XA);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq == 0);
    CHECK_EQ_INT(g->npending, GATE_TRAIN_PENDING);

    /* Proof is still recorded, up to 4096 waiting requests in all. */
    for (i = 0; i < 4; i++) train(g, now, &XA, 0);
    CHECK(g->candidate != NULL);
    CHECK(gateEvaluate(g, &XA, 0.0, &ev) == GATE_OK);
    CHECK_EQ_INT(gatePredictNeedsFeatures(g, U_PLAIN, &ev), 1);
    for (i = GATE_TRAIN_PENDING; i < GATE_MAX_PENDING; i++) {
        r = askText(g, now, U_PLAIN, &XA);
        CHECK(r.seq != 0);
    }
    CHECK(gateFull(g));
    CHECK_EQ_INT(g->nevidence, GATE_MAX_PENDING - GATE_TRAIN_PENDING);
    CHECK_EQ_INT(gatePredictNeedsFeatures(g, U_PLAIN, &ev), 0);
    r = askText(g, now, U_PLAIN, &XA);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq == 0);
    CHECK_EQ_INT(g->stats.full, 1);
    gateFree(g);
}

/* What makes a gate full is the number of requests still waiting for their
 * answer. Answers that wait for their turn behind an older request are not
 * counted: they are bounded by GATE_MAX_WAITING. */
TEST(full_counts_unanswered) {
    int64_t now = T0;
    gate *g = activeScoreGate(now);
    gatePredictResult r;
    uint64_t seq[GATE_MAX_PENDING];
    uint32_t i;

    for (i = 0; i < GATE_MAX_PENDING; i++) {
        r = ask(g, now, U_SPOT, 0.9);
        CHECK(r.action == GATE_ACTION_AUDIT && r.seq != 0);
        seq[i] = r.seq;
    }
    CHECK(gateFull(g));
    CHECK_EQ_INT(gateUnanswered(g), GATE_MAX_PENDING);
    /* The answers of the newest 100 arrive and wait behind the 3996 older
     * requests: there is room for 100 requests again. */
    for (i = GATE_MAX_PENDING - 100; i < GATE_MAX_PENDING; i++)
        CHECK_EQ_INT(answer(g, now, seq[i], 1), 1);
    CHECK_EQ_INT(g->npending, GATE_MAX_PENDING);
    CHECK_EQ_INT(g->nanswered, 100);
    CHECK_EQ_INT(gateUnanswered(g), GATE_MAX_PENDING - 100);
    CHECK_EQ_INT(servingOf(g)->samples, 35); /* as proof they wait... */
    CHECK_EQ_INT(servingOf(g)->audits, 100); /* ...the drift detector has heard them */
    CHECK(!gateFull(g));
    for (i = 1; i <= 100; i++) {
        r = ask(g, now, U_SPOT, 0.9);
        CHECK(r.action == GATE_ACTION_AUDIT && r.seq != 0);
        CHECK_EQ_INT(gateFull(g), i == 100);
    }
    CHECK_EQ_INT(g->npending, GATE_MAX_PENDING + 100);
    CHECK_EQ_INT(gateUnanswered(g), GATE_MAX_PENDING);
    r = ask(g, now, U_PLAIN, 0.9);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq == 0);
    CHECK_EQ_INT(g->stats.full, 1);
    /* The oldest answer: applied at once, and one more request fits. */
    CHECK_EQ_INT(answer(g, now, seq[0], 1), 1);
    CHECK_EQ_INT(servingOf(g)->samples, 36);
    CHECK_EQ_INT(servingOf(g)->audits, 101);
    CHECK_EQ_INT(g->npending, GATE_MAX_PENDING + 99);
    CHECK_EQ_INT(gateUnanswered(g), GATE_MAX_PENDING - 1);
    CHECK(!gateFull(g));
    CHECK(ask(g, now, U_PLAIN, 0.9).action == GATE_ACTION_ACCEPT);
    gateFree(g);
}

/* The limit on requests kept for training counts unanswered requests too. */
TEST(training_limit_counts_unanswered) {
    int64_t now = T0;
    gate *g = trainedGate(4000);
    gatePredictResult r, head, kept;
    gateEval ev;
    uint32_t i;

    /* Generation 1 answers label0 from threshold 2 (0.665): ten wrong
     * guesses at 0.6, 39 correct ones at 0.99. It has no threshold for
     * label1. No other snapshot is due for 4000 examples. */
    for (i = 0; i < 10; i++) {
        ev = guesses(g, -1, 0.0, 0, 0.6);
        r = askGuess(g, now, U_PLAIN, &ev, &XA);
        CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
    }
    for (i = 0; i < 39; i++) {
        ev = guesses(g, -1, 0.0, 0, 0.99);
        r = askGuess(g, now, U_PLAIN, &ev, &XA);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
    }
    CHECK(g->serving != NULL && servingOf(g)->policy[0] == 2 && g->candidate == NULL);

    /* Proof for label1: an open request and 50 answers that wait behind
     * it. */
    ev = guesses(g, 1, 0.8, -1, 0.0);
    head = askGuess(g, now, U_PLAIN, &ev, &XB);
    CHECK_EQ_INT(flagsOf(g, head.seq), GATE_OBS_SERV);
    for (i = 0; i < 50; i++) {
        r = askGuess(g, now, U_PLAIN, &ev, &XB);
        CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
    }
    CHECK_EQ_INT(g->nanswered, 50);
    /* Guesses of label0 under the threshold are not answered and are no
     * proof: they are kept for training, while fewer than 3072 requests
     * wait for their answer. One does already. */
    ev = guesses(g, 0, 0.6, -1, 0.0);
    memset(&kept, 0, sizeof(kept));
    for (i = 1; i < GATE_TRAIN_PENDING; i++) {
        CHECK_EQ_INT(gatePredictNeedsFeatures(g, U_PLAIN, &ev), 1);
        kept = askGuess(g, now, U_PLAIN, &ev, &XA);
        if (kept.action != GATE_ACTION_ABSTAIN || kept.seq == 0) break;
    }
    CHECK_EQ_INT(i, GATE_TRAIN_PENDING);
    CHECK_EQ_INT(flagsOf(g, kept.seq), 0);
    CHECK_EQ_INT(gateUnanswered(g), GATE_TRAIN_PENDING);
    CHECK_EQ_INT(g->npending, GATE_TRAIN_PENDING + 50);
    CHECK_EQ_INT(gatePredictNeedsFeatures(g, U_PLAIN, &ev), 0);
    r = askGuess(g, now, U_PLAIN, &ev, &XA);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq == 0);
    CHECK_EQ_INT(g->stats.full, 0);
    /* An answer makes room for one. */
    CHECK_EQ_INT(answer(g, now, kept.seq, 0), 1);
    CHECK_EQ_INT(gatePredictNeedsFeatures(g, U_PLAIN, &ev), 1);
    CHECK(askGuess(g, now, U_PLAIN, &ev, &XA).seq != 0);
    CHECK(askGuess(g, now, U_PLAIN, &ev, &XA).seq == 0);
    /* Proof is still recorded: until 4096 requests wait for their answer. */
    ev = guesses(g, 1, 0.8, -1, 0.0);
    for (i = GATE_TRAIN_PENDING; i < GATE_MAX_PENDING; i++) {
        CHECK(!gateFull(g));
        if (askGuess(g, now, U_PLAIN, &ev, &XB).seq == 0) break;
    }
    CHECK_EQ_INT(i, GATE_MAX_PENDING);
    CHECK(gateFull(g));
    CHECK_EQ_INT(g->npending, GATE_MAX_PENDING + 50);
    CHECK_EQ_INT(servingOf(g)->regions[1 * T].n, 0); /* the 50 still wait */
    gateFree(g);
}

TEST(full_by_bytes) {
    /* The largest clue vector there is: all 8192 buckets. */
    const uint64_t per = sizeof(gateObs) + (uint64_t)GATE_DIM * (sizeof(uint32_t) + sizeof(float));
    const uint32_t fit = (uint32_t)((GATE_MAX_PENDING_BYTES + per - 1) / per);
    /* What the last of them may bring to land on the limit exactly. */
    const uint64_t room = GATE_MAX_PENDING_BYTES - (fit - 1) * per - sizeof(gateObs);
    const int exact = room % 8 == 0 && room <= 8 * (uint64_t)GATE_DIM;
    gate *g = classGate(2, 4);
    gatePredictResult r;
    gateSparse big;
    int64_t now = T0;
    uint64_t first = 0;
    uint32_t i;

    big.n = GATE_DIM;
    big.idx = malloc(sizeof(uint32_t) * GATE_DIM);
    big.val = malloc(sizeof(float) * GATE_DIM);
    if (big.idx == NULL || big.val == NULL) die("out of memory");
    for (i = 0; i < GATE_DIM; i++) {
        big.idx[i] = i;
        big.val[i] = 0.001f;
    }
    for (i = 0; i < 4; i++) train(g, now, &XA, 0);

    /* 16 MiB of stored clues is 256 such requests, far below 4096. The
     * last one is cut down so that the gate holds the limit to the byte:
     * that is full already. */
    CHECK_EQ_INT(fit, 256);
    for (i = 1; i <= fit; i++) {
        if (i == fit && exact) big.n = (uint32_t)(room / 8);
        r = askText(g, now, U_PLAIN, &big);
        CHECK(r.seq != 0);
        CHECK_EQ_INT(gateFull(g), i == fit);
        if (i == 1) first = r.seq;
    }
    big.n = GATE_DIM;
    CHECK_EQ_INT(g->npending, fit);
    if (exact) CHECK_EQ_INT(g->pending_bytes, GATE_MAX_PENDING_BYTES);
    CHECK_EQ_INT(gateMemoryUsage(g), expectBytes(g));
    r = askText(g, now, U_PLAIN, &XA);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq == 0);
    CHECK_EQ_INT(g->stats.full, 1);
    /* An answer releases the clues it came for. */
    CHECK_EQ_INT(answer(g, now, first, 0), 1);
    CHECK(!gateFull(g));
    CHECK(askText(g, now, U_PLAIN, &XA).seq != 0);
    gateFree(g);
    free(big.idx);
    free(big.val);
}

/* ------------------------- Pause, reset, recovery ------------------------ */

TEST(pause) {
    gate *g = scoreGate();
    gatePredictResult r;
    int64_t now = T0;
    uint32_t i;

    gatePause(g, now);
    CHECK(gateState(g) == GATE_STATE_PAUSED);
    /* Shadow mode: the proof goes on and the snapshot is promoted. */
    for (i = 0; i < 35; i++) feed(g, now, U_PLAIN, 0.9, 1);
    CHECK(g->serving != NULL && servingOf(g)->policy[0] == 0);
    CHECK_EQ_INT(g->stats.promotions, 1);
    CHECK(gateState(g) == GATE_STATE_PAUSED);
    CHECK(gateRowThreshold(g, 0) == 0.0);

    /* What it would answer, it does not; spot checks are still recorded
     * and still watched by the drift detector. */
    r = ask(g, now, U_PLAIN, 0.9);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq == 0);
    r = ask(g, now, U_SPOT, 0.9);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0);
    CHECK_EQ_INT(flagsOf(g, r.seq), GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_SERV);
    CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
    CHECK_EQ_INT(servingOf(g)->audits, 1);
    CHECK_EQ_INT(servingOf(g)->audit_errors, 1);
    CHECK_EQ_INT(g->stats.accepted, 0);
    CHECK_EQ_INT(g->stats.audited, 0);
    CHECK_EQ_INT(g->stats.abstained, 37);
    CHECK(gateCoverage(g) == 1.0); /* what it could answer */

    gateResume(g, now);
    CHECK(gateState(g) == GATE_STATE_ACTIVE);
    CHECK(ask(g, now, U_PLAIN, 0.9).action == GATE_ACTION_ACCEPT);
    CHECK(ask(g, now, U_SPOT, 0.9).action == GATE_ACTION_AUDIT);
    gatePause(g, now);
    gatePause(g, now);
    CHECK(gateState(g) == GATE_STATE_PAUSED);
    CHECK(ask(g, now, U_PLAIN, 0.9).action == GATE_ACTION_ABSTAIN);
    /* GRESET keeps the switch where it is. */
    gateReset(g, now, EPOCH2);
    CHECK(gateState(g) == GATE_STATE_PAUSED);
    CHECK(g->serving == NULL && g->candidate != NULL);
    gateResume(g, now);
    gateResume(g, now);
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    inv(g);
    gateFree(g);
}

/* ACTIVE generation 1, candidate generation 2, two requests waiting. */
static gate *busyClassifier(int64_t now, gatePredictResult *waiting) {
    gate *g = trainedGate(12);
    uint32_t i;

    for (i = 0; i < 39; i++) feedText(g, now, U_PLAIN, &XA, 0);
    CHECK(g->serving != NULL && servingOf(g)->gen == 1);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 2);
    waiting[0] = askText(g, now, U_SPOT, &XA);
    waiting[1] = askText(g, now, U_PLAIN, &XB);
    CHECK(waiting[0].seq != 0 && waiting[1].seq != 0);
    CHECK(askText(g, now, U_PLAIN, &XA).action == GATE_ACTION_ACCEPT);
    return g;
}

TEST(reset) {
    static const gateCounters zero;
    gatePredictResult waiting[2];
    int64_t now = T0 + 500;
    gate *g = busyClassifier(now, waiting);
    uint64_t next_seq = g->next_seq;
    uint32_t i, nonzero = 0;

    gateReset(g, now - 400, EPOCH2); /* sets the clock: here, back */
    inv(g);
    CHECK(gateState(g) == GATE_STATE_COLD);
    CHECK(g->candidate == NULL && g->serving == NULL);
    CHECK_EQ_INT(g->npending, 0);
    CHECK_EQ_INT(g->nevidence, 0);
    CHECK_EQ_INT(g->pending_bytes, 0);
    CHECK(g->oldest == NULL && g->newest == NULL && g->ehead == NULL && g->etail == NULL);
    CHECK(memcmp(&g->stats, &zero, sizeof(zero)) == 0);
    CHECK_EQ_INT(g->trainer.updates, 0);
    for (i = 0; i < g->trainer.nvec * g->trainer.dim; i++) nonzero += g->trainer.w[i] != 0.0f;
    CHECK_EQ_INT(nonzero + (g->trainer.b[0] != 0.0f), 0);
    /* Kept: configuration, identity, id counter and the ledger. The clock
     * is the one the reset was given. */
    CHECK_EQ_INT(g->generations, 2);
    CHECK_NEAR(gateBudgetSpentNow(g), DELTA * 2.0 / 3.0, 1e-15);
    CHECK(g->lineage == LINEAGE && g->epoch == EPOCH2);
    CHECK(g->created_ms == T0 && g->now_ms == now - 400);
    CHECK_EQ_INT(g->next_seq, next_seq);
    CHECK_EQ_INT(g->cfg.nclasses, 2);
    CHECK_EQ_STR(g->cfg.labels[1], "label1");
    CHECK_EQ_INT(g->cfg.snapshot_examples, 12);
    CHECK_EQ_INT(gateLearn(g, now, EPOCH, waiting[0].seq, 0), 0);
    CHECK_EQ_INT(gateLearn(g, now, EPOCH2, waiting[1].seq, 1), 0);

    /* The next snapshot is number 3: 1 and 2 were used. */
    for (i = 0; i < 12; i++) {
        CHECK(g->candidate == NULL);
        train(g, now, (i & 1) ? &XB : &XA, (int32_t)(i & 1));
    }
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 3);
    CHECK_NEAR(candidateOf(g)->log_threshold, log(12.0 * 64.0 / 0.2), 1e-12);
    gateFree(g);

    /* A score gate starts its next test right away. */
    g = activeScoreGate(now);
    CHECK(ask(g, now, U_SPOT, 0.9).seq != 0);
    gateReset(g, now, EPOCH2);
    inv(g);
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    CHECK(g->serving == NULL && g->candidate != NULL && candidateOf(g)->gen == 2);
    CHECK_EQ_INT(candidateOf(g)->samples, 0);
    CHECK_EQ_INT(g->npending, 0);
    CHECK(memcmp(&g->stats, &zero, sizeof(zero)) == 0);
    CHECK(ask(g, now, U_PLAIN, 0.9).action == GATE_ACTION_ABSTAIN);
    gateFree(g);
}

TEST(recover) {
    gatePredictResult waiting[2];
    int64_t now = T0 + 500;
    gate *g = busyClassifier(now, waiting);
    gateCounters st = g->stats;
    uint64_t weights = mixModel(0, &g->trainer, 0);
    uint32_t i;

    CHECK_EQ_INT(g->trainer.updates, 51);
    gateRecover(g, now - 400, EPOCH2);
    inv(g);
    /* The proof and the waiting requests are gone... */
    CHECK(g->serving == NULL);
    CHECK_EQ_INT(g->npending, 0);
    CHECK_EQ_INT(g->pending_bytes, 0);
    CHECK_EQ_INT(gateLearn(g, now, EPOCH, waiting[0].seq, 0), 0);
    CHECK_EQ_INT(gateLearn(g, now, EPOCH2, waiting[1].seq, 1), 0);
    /* ...the model is not, so it is frozen again on the spot, under the
     * next number... */
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 3);
    CHECK_EQ_INT(g->generations, 3);
    CHECK_EQ_INT(candidateOf(g)->samples, 0);
    CHECK_EQ_INT(candidateOf(g)->frozen_ms, now - 400); /* the clock it was given */
    CHECK_EQ_INT(g->trainer.updates, 51);
    CHECK(mixModel(0, &g->trainer, 0) == weights);
    CHECK(mixModel(0, &candidateOf(g)->model, 0) == weights);
    CHECK_EQ_INT(g->snapshot_updates, 51);
    /* ...and the counters are history, not proof. */
    CHECK_EQ_INT(g->stats.requests, st.requests);
    CHECK_EQ_INT(g->stats.accepted, st.accepted);
    CHECK_EQ_INT(g->stats.learned, st.learned);
    CHECK_EQ_INT(g->stats.observations, st.observations);
    CHECK_EQ_INT(g->stats.promotions, 1);
    CHECK_EQ_INT(g->stats.ignored, st.ignored + 2);
    CHECK(gateCoverage(g) == 0.0);
    CHECK(g->lineage == LINEAGE && g->epoch == EPOCH2 && g->now_ms == now);
    gateFree(g);

    /* A model that has not seen enough examples yet has nothing to freeze;
     * it counts on from where it was. */
    g = classGate(2, 12);
    for (i = 0; i < 7; i++) train(g, now, &XA, 0);
    gateRecover(g, now, EPOCH2);
    CHECK(gateState(g) == GATE_STATE_COLD);
    CHECK_EQ_INT(g->trainer.updates, 7);
    for (i = 0; i < 5; i++) {
        CHECK(g->candidate == NULL);
        train(g, now, &XB, 1);
    }
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 1);
    gateFree(g);

    g = activeScoreGate(now);
    st = g->stats;
    gateRecover(g, now, EPOCH2);
    inv(g);
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    CHECK(g->serving == NULL && g->candidate != NULL && candidateOf(g)->gen == 2);
    CHECK_EQ_INT(g->stats.requests, st.requests);
    CHECK_EQ_INT(g->stats.promotions, 1);
    gateFree(g);
}

/* --------------------- Clues: needed, taken, refused --------------------- */

/* One request, checking the three things a caller relies on: whether the
 * clues were announced as needed, whether the request was recorded, and
 * who owns the clues afterwards. */
static gatePredictResult askOwned(gate *g, int64_t now, double u, int expect_recorded) {
    gatePredictResult r;
    gateSparse x;
    uint32_t *idx;
    float *val;
    gateEval ev;
    int needs;

    gateMaintain(g, now);
    CHECK(gateEvaluateFor(g, &XA, 0.0, u, &ev) == GATE_OK);
    needs = gatePredictNeedsFeatures(g, u, &ev);
    if (gateSparseCopy(&x, &XA) != GATE_OK) die("out of memory");
    idx = x.idx;
    val = x.val;
    CHECK(gatePredict(g, now, u, &ev, &x, &r) == GATE_OK);
    CHECK_EQ_INT(needs, expect_recorded);
    CHECK_EQ_INT(r.seq != 0, expect_recorded);
    if (r.seq != 0) {
        const gateObs *o = gateFindObservation(g, r.seq);

        CHECK(x.n == 0 && x.idx == NULL && x.val == NULL);
        CHECK(o != NULL && o->x.n == 1 && o->x.idx == idx && o->x.val == val);
    } else {
        CHECK(x.n == 1 && x.idx == idx && x.val == val);
        gateSparseFree(&x);
    }
    inv(g);
    return r;
}

TEST(clues_needed_and_owned) {
    gate *g = classGate(2, 12);
    gatePredictResult r;
    int64_t now = T0;
    gateEval ev;
    uint32_t i;

    askOwned(g, now, U_PLAIN, 1); /* COLD: recorded for training */
    for (i = 0; i < 12; i++) train(g, now, (i & 1) ? &XB : &XA, (int32_t)(i & 1));
    for (i = 0; i < 39; i++) {
        r = askOwned(g, now, U_PLAIN, 1); /* CALIBRATING: proof */
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
    }
    CHECK(gateState(g) == GATE_STATE_ACTIVE);
    r = askOwned(g, now, U_PLAIN, 0);
    CHECK(r.action == GATE_ACTION_ACCEPT);
    r = askOwned(g, now, U_SPOT, 1);
    CHECK(r.action == GATE_ACTION_AUDIT);
    /* An ACCEPT needs no clues at all. */
    CHECK(gateEvaluate(g, &XA, 0.0, &ev) == GATE_OK);
    CHECK(gatePredict(g, now, U_PLAIN, &ev, NULL, &r) == GATE_OK);
    CHECK(r.action == GATE_ACTION_ACCEPT && r.label == 0);
    gatePause(g, now);
    r = askOwned(g, now, U_PLAIN, 1); /* not answered: the label trains */
    CHECK(r.action == GATE_ACTION_ABSTAIN);
    gateFree(g);

    /* A score gate never has clues. */
    g = scoreGate();
    CHECK(gateEvaluate(g, NULL, 0.9, &ev) == GATE_OK);
    CHECK_EQ_INT(gatePredictNeedsFeatures(g, U_SPOT, &ev), 0);
    CHECK_EQ_INT(gatePredictNeedsFeatures(g, U_PLAIN, &ev), 0);
    gateFree(g);
}

TEST(predict_refuses_bad_input) {
    gate *g = classGate(2, 12);
    gatePredictResult r, waiting;
    int64_t now = T0;
    uint64_t before;
    gateEval ev, bad;
    gateSparse x;
    uint32_t i;

    /* Clues are needed and there are none: an error, and nothing happened
     * except the maintenance every call starts with. */
    waiting = askText(g, now, U_PLAIN, &XB);
    CHECK(gateEvaluate(g, &XA, 0.0, &ev) == GATE_OK);
    before = fingerprint(g, 0);
    CHECK(gatePredict(g, now, U_PLAIN, &ev, NULL, &r) == GATE_ERR);
    CHECK(r.seq == 0);
    CHECK(fingerprint(g, 0) == before);
    inv(g);
    now += TIMEOUT;
    CHECK(gatePredict(g, now, U_PLAIN, &ev, NULL, &r) == GATE_ERR);
    CHECK(gateFindObservation(g, waiting.seq) == NULL);
    CHECK_EQ_INT(g->stats.dropped, 1);
    CHECK_EQ_INT(g->stats.requests, 1);
    CHECK_EQ_INT(g->now_ms, now);
    inv(g);

    /* Guesses that do not belong to the generations the gate has. */
    for (i = 0; i < 12; i++) train(g, now, (i & 1) ? &XB : &XA, (int32_t)(i & 1));
    for (i = 0; i < 39; i++) feedText(g, now, U_PLAIN, &XA, 0);
    CHECK(g->serving != NULL && g->candidate != NULL);
    CHECK(gateEvaluate(g, &XA, 0.0, &ev) == GATE_OK);
    before = fingerprint(g, 0);
    if (gateSparseCopy(&x, &XA) != GATE_OK) die("out of memory");
    bad = ev;
    bad.has_serving = 0;
    CHECK(gatePredict(g, now, U_SPOT, &bad, &x, &r) == GATE_ERR);
    CHECK_EQ_INT(gatePredictNeedsFeatures(g, U_SPOT, &bad), 0);
    bad = ev;
    bad.has_candidate = 0;
    CHECK(gatePredict(g, now, U_SPOT, &bad, &x, &r) == GATE_ERR);
    bad = ev;
    bad.serving.label = 2; /* there are two labels */
    CHECK(gatePredict(g, now, U_SPOT, &bad, &x, &r) == GATE_ERR);
    bad.serving.label = -1;
    CHECK(gatePredict(g, now, U_SPOT, &bad, &x, &r) == GATE_ERR);
    bad = ev;
    bad.candidate.label = 2;
    CHECK(gatePredict(g, now, U_SPOT, &bad, &x, &r) == GATE_ERR);
    bad.candidate.label = -7;
    CHECK(gatePredict(g, now, U_SPOT, &bad, &x, &r) == GATE_ERR);
    CHECK(fingerprint(g, 0) == before);
    CHECK(x.n == 1 && x.idx != NULL);
    gateSparseFree(&x);
    inv(g);

    /* GADD: a label the gate does not have. */
    CHECK(gateTrain(g, now, &XA, 2) == GATE_ERR);
    CHECK(gateTrain(g, now, &XA, -1) == GATE_ERR);
    CHECK(fingerprint(g, 0) == before);
    gateFree(g);

    g = scoreGate();
    before = fingerprint(g, 0);
    CHECK(gateTrain(g, T0, &XA, 0) == GATE_ERR); /* nothing to train */
    CHECK(fingerprint(g, 0) == before);
    gateFree(g);
}

/* ----------------------- Clock, maintenance, memory ---------------------- */

TEST(clock) {
    gate *g = scoreGate();
    gatePredictResult r;
    const gateObs *o;

    r = ask(g, T0 + 100, U_PLAIN, 0.9);
    o = gateFindObservation(g, r.seq);
    CHECK(o != NULL && o->created_ms == T0 + 100);
    CHECK_EQ_INT(g->now_ms, T0 + 100);

    /* A clock that jumps back is ignored: the gate stays at the largest
     * value it has seen, whichever call brings the older one. */
    r = ask(g, T0 + 50, U_PLAIN, 0.9);
    o = gateFindObservation(g, r.seq);
    CHECK(o != NULL && o->created_ms == T0 + 100);
    CHECK_EQ_INT(g->now_ms, T0 + 100);
    gateMaintain(g, T0);
    gatePause(g, T0 - 5000);
    gateResume(g, 0);
    CHECK_EQ_INT(gateLearn(g, T0 + 1, EPOCH, 999, 1), 0);
    CHECK_EQ_INT(g->now_ms, T0 + 100);
    CHECK_EQ_INT(g->npending, 2);

    /* The feedback window is measured on that clock. */
    CHECK_EQ_INT(gateMaintenanceDue(g, T0 + 50), 0);
    CHECK_EQ_INT(gateMaintenanceDue(g, T0 + 100 + TIMEOUT - 1), 0);
    CHECK_EQ_INT(gateMaintenanceDue(g, T0 + 100 + TIMEOUT), 1);
    gateMaintain(g, T0 + 100 + TIMEOUT - 1);
    CHECK_EQ_INT(g->npending, 2);
    gateMaintain(g, T0 + 100 + TIMEOUT);
    CHECK_EQ_INT(g->npending, 0);
    CHECK_EQ_INT(g->stats.missing, 2);
    /* The two exceptions: a recovery and a reset drop everything that
     * carries a time, and set the clock to the one they are given. */
    gateRecover(g, T0 + 30, EPOCH2);
    CHECK_EQ_INT(g->now_ms, T0 + 30);
    CHECK_EQ_INT(candidateOf(g)->frozen_ms, T0 + 30);
    r = ask(g, T0 + 20, U_PLAIN, 0.9); /* from there it only moves forward again */
    o = gateFindObservation(g, r.seq);
    CHECK(o != NULL && o->created_ms == T0 + 30);
    gateReset(g, T0, EPOCH3);
    CHECK_EQ_INT(g->now_ms, T0);
    CHECK_EQ_INT(candidateOf(g)->frozen_ms, T0);
    CHECK_EQ_INT(g->created_ms, T0);
    gateReset(g, T0 + 77, EPOCH);
    CHECK_EQ_INT(g->now_ms, T0 + 77);
    inv(g);
    gateFree(g);

    g = classGate(2, 12);
    train(g, T0 + 7, &XA, 0);
    CHECK_EQ_INT(g->now_ms, T0 + 7);
    train(g, T0, &XA, 0);
    CHECK_EQ_INT(g->now_ms, T0 + 7);
    gateFree(g);
}

/* A replica that used to be a primary may hold a later clock than its new
 * primary. RESET and RECOVER bring it back: from then on two gates that are
 * told the same things are the same, clock and timeouts included. */
TEST(reset_and_recover_set_the_clock) {
    int reset;

    for (reset = 0; reset < 2; reset++) {
        gate *a = scoreGate(), *b = scoreGate();
        gatePredictResult ra, rb;
        uint32_t i;

        gateMaintain(b, T0 + 10000); /* b has been ten seconds ahead */
        CHECK(fingerprint(a, 0) != fingerprint(b, 0));
        if (reset) {
            gateReset(a, T0 + 5, EPOCH2);
            gateReset(b, T0 + 5, EPOCH2);
        } else {
            gateRecover(a, T0 + 5, EPOCH2);
            gateRecover(b, T0 + 5, EPOCH2);
        }
        inv(b);
        CHECK_EQ_INT(a->now_ms, T0 + 5);
        CHECK_EQ_INT(b->now_ms, T0 + 5);
        CHECK_EQ_INT(candidateOf(b)->frozen_ms, T0 + 5);
        CHECK(fingerprint(a, 0) == fingerprint(b, 0));
        /* Requests are stamped alike, and their time is up at the same
         * moment. */
        for (i = 0; i < 10; i++) {
            ra = ask(a, T0 + 100 + i, U_PLAIN, 0.9);
            rb = ask(b, T0 + 100 + i, U_PLAIN, 0.9);
            CHECK(ra.seq == rb.seq && ra.seq != 0);
            if (i & 1) {
                CHECK_EQ_INT(answer(a, T0 + 100 + i, ra.seq, 1), 1);
                CHECK_EQ_INT(answer(b, T0 + 100 + i, rb.seq, 1), 1);
            }
        }
        CHECK(gateFindObservation(b, ra.seq - 9) != NULL &&
              gateFindObservation(b, ra.seq - 9)->created_ms == T0 + 100);
        CHECK(fingerprint(a, 0) == fingerprint(b, 0));
        expectIdle(b, T0 + 100 + GRACE - 1);
        expectDue(b, T0 + 100 + GRACE);
        expectDue(b, T0 + 100 + TIMEOUT);
        gateMaintain(a, T0 + 100 + TIMEOUT);
        CHECK(fingerprint(a, 0) == fingerprint(b, 0));
        CHECK_EQ_INT(b->stats.missing, 1);
        gateFree(a);
        gateFree(b);
    }
}

/* The clock contract (gate_core.h, TIME): 0 <= now <= INT64_MAX -
 * GATE_MAX_FEEDBACK_TIMEOUT_MS. Timeouts are compared as ages, so nothing
 * overflows at either end of that range (run this under UBSan). */
TEST(clock_at_the_ends) {
    const int64_t top = INT64_MAX - GATE_MAX_FEEDBACK_TIMEOUT_MS;
    const int64_t week = GATE_MAX_FEEDBACK_TIMEOUT_MS, grace = week / 4;
    gatePredictResult h, a, r;
    gateConfig cfg;
    char err[128];
    gate *g;

    /* The longest timeout there is, on a request whose time is up at the
     * very last moment of the clock. */
    baseConfig(&cfg);
    cfg.feedback_timeout_ms = week;
    if (gateConfigValidate(&cfg, err, sizeof(err)) != GATE_OK) die(err);
    g = gateCreate(&cfg, LINEAGE, EPOCH, top - week - 5);
    if (g == NULL) die("gateCreate failed");
    gateConfigFree(&cfg);
    h = ask(g, top - week, U_PLAIN, 0.9);
    a = ask(g, top - week + 1, U_PLAIN, 0.9);
    CHECK_EQ_INT(answer(g, top - week + 2, a.seq, 1), 1);
    expectIdle(g, top - week + grace - 1);
    CHECK_EQ_INT(gateMaintenanceDue(g, top), 1);
    expectDue(g, top - week + grace);
    CHECK_EQ_INT(flagsOf(g, h.seq), GATE_OBS_CAND | GATE_OBS_PROVISIONAL);
    CHECK_EQ_INT(candidateOf(g)->samples, 2);
    expectIdle(g, top - 1);
    expectDue(g, top);
    CHECK_EQ_INT(g->stats.missing, 1);
    CHECK_EQ_INT(g->npending, 0);
    CHECK_EQ_INT(g->now_ms, top);
    /* Every operation at the last moment itself. */
    r = ask(g, top, U_PLAIN, 0.9);
    CHECK(r.seq != 0 && gateFindObservation(g, r.seq)->created_ms == top);
    CHECK_EQ_INT(gateMaintenanceDue(g, top), 0);
    CHECK_EQ_INT(answer(g, top, r.seq, 1), 1);
    CHECK_EQ_INT(candidateOf(g)->samples, 3);
    r = ask(g, top, U_PLAIN, 0.9);
    gatePause(g, top);
    gateResume(g, top);
    expectIdle(g, top);
    gateRecover(g, top, EPOCH2);
    inv(g);
    CHECK(candidateOf(g)->gen == 2 && candidateOf(g)->frozen_ms == top);
    gateReset(g, top, EPOCH3);
    inv(g);
    CHECK(candidateOf(g)->gen == 3 && g->now_ms == top);
    gateFree(g);

    /* A short timeout near the end, and a gate created at the end. */
    g = scoreGate();
    h = ask(g, top - TIMEOUT, U_PLAIN, 0.9);
    a = ask(g, top - TIMEOUT, U_PLAIN, 0.9);
    CHECK_EQ_INT(answer(g, top - TIMEOUT, a.seq, 1), 1);
    expectIdle(g, top - TIMEOUT + GRACE - 1);
    expectDue(g, top - TIMEOUT + GRACE);
    expectIdle(g, top - 1);
    expectDue(g, top);
    CHECK_EQ_INT(g->stats.missing, 1);
    gateFree(g);
    baseConfig(&cfg);
    if (gateConfigValidate(&cfg, err, sizeof(err)) != GATE_OK) die(err);
    g = gateCreate(&cfg, LINEAGE, EPOCH, top);
    if (g == NULL) die("gateCreate failed");
    gateConfigFree(&cfg);
    inv(g);
    CHECK(g->created_ms == top && g->now_ms == top && candidateOf(g)->frozen_ms == top);
    feed(g, top, U_PLAIN, 0.9, 1);
    h = ask(g, top, U_PLAIN, 0.9);
    CHECK_EQ_INT(gateMaintenanceDue(g, top), 0);
    CHECK_EQ_INT(gateMaintenanceDue(g, 0), 0);
    CHECK_EQ_INT(candidateOf(g)->samples, 1);
    gateFree(g);

    /* The beginning: a gate created at 0, with the longest timeout. */
    baseConfig(&cfg);
    cfg.feedback_timeout_ms = week;
    if (gateConfigValidate(&cfg, err, sizeof(err)) != GATE_OK) die(err);
    g = gateCreate(&cfg, LINEAGE, EPOCH, 0);
    if (g == NULL) die("gateCreate failed");
    gateConfigFree(&cfg);
    h = ask(g, 0, U_PLAIN, 0.9);
    a = ask(g, 0, U_PLAIN, 0.9);
    CHECK_EQ_INT(answer(g, 0, a.seq, 1), 1);
    CHECK(gateFindObservation(g, h.seq)->created_ms == 0 && g->now_ms == 0);
    expectIdle(g, grace - 1);
    expectDue(g, grace);
    expectIdle(g, week - 1);
    expectDue(g, week);
    CHECK_EQ_INT(g->stats.missing, 1);
    /* From the first moment to the last in one step. */
    h = ask(g, week, U_PLAIN, 0.9);
    expectDue(g, top);
    CHECK_EQ_INT(g->stats.missing, 2);
    CHECK_EQ_INT(g->now_ms, top);
    gateFree(g);
}

TEST(maintenance_due) {
    gateConfig cfg;
    gate *g = scoreGate();
    gatePredictResult r;
    uint64_t before;

    /* Nothing waiting, nothing to freeze: no reason to wake up, ever. */
    CHECK_EQ_INT(gateMaintenanceDue(g, T0), 0);
    CHECK_EQ_INT(gateMaintenanceDue(g, T0 + 1000 * TIMEOUT), 0);
    before = fingerprint(g, SKIP_CLOCK);
    gateMaintain(g, T0 + 1000 * TIMEOUT);
    CHECK(fingerprint(g, SKIP_CLOCK) == before);
    CHECK_EQ_INT(g->now_ms, T0 + 1000 * TIMEOUT);

    /* The oldest waiting request decides; asking does not change a thing. */
    r = ask(g, g->now_ms, U_PLAIN, 0.9);
    ask(g, g->now_ms + 300, U_PLAIN, 0.9);
    before = fingerprint(g, 0);
    CHECK_EQ_INT(gateMaintenanceDue(g, g->now_ms + TIMEOUT - 301), 0);
    CHECK_EQ_INT(gateMaintenanceDue(g, g->now_ms + TIMEOUT - 300), 1);
    CHECK_EQ_INT(gateMaintenanceDue(g, 0), 0); /* an old clock means "now" */
    CHECK(fingerprint(g, 0) == before);
    CHECK_EQ_INT(answer(g, g->now_ms, r.seq, 1), 1);
    CHECK_EQ_INT(gateMaintenanceDue(g, g->now_ms + TIMEOUT - 300), 0);
    CHECK_EQ_INT(gateMaintenanceDue(g, g->now_ms + TIMEOUT), 1);
    gateFree(g);

    /* Proof that is due to move on without a late answer is a reason too:
     * an open request, an answer behind it, a quarter of the timeout. */
    g = scoreGate();
    r = ask(g, T0, U_PLAIN, 0.9);
    CHECK_EQ_INT(answer(g, T0, ask(g, T0, U_PLAIN, 0.9).seq, 1), 1);
    before = fingerprint(g, 0);
    CHECK_EQ_INT(gateMaintenanceDue(g, T0 + GRACE - 1), 0);
    CHECK_EQ_INT(gateMaintenanceDue(g, T0 + GRACE), 1);
    CHECK_EQ_INT(gateMaintenanceDue(g, T0 + TIMEOUT), 1);
    CHECK(fingerprint(g, 0) == before);
    gateMaintain(g, T0 + GRACE);
    inv(g);
    CHECK_EQ_INT(flagsOf(g, r.seq), GATE_OBS_CAND | GATE_OBS_PROVISIONAL);
    /* That is done once. Then only the feedback timeout is left. */
    CHECK_EQ_INT(gateMaintenanceDue(g, T0 + GRACE), 0);
    CHECK_EQ_INT(gateMaintenanceDue(g, T0 + TIMEOUT - 1), 0);
    CHECK_EQ_INT(gateMaintenanceDue(g, T0 + TIMEOUT), 1);
    gateFree(g);

    /* A gate that should be testing a snapshot and has none (as after a
     * decode): due at any time, until maintained. */
    baseConfig(&cfg);
    g = gateAllocate(&cfg);
    if (g == NULL) die("out of memory");
    gateConfigFree(&cfg);
    CHECK(g->candidate == NULL && g->serving == NULL);
    CHECK_EQ_INT(gateMaintenanceDue(g, 0), 1);
    gateMaintain(g, 5);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 1);
    CHECK_EQ_INT(gateMaintenanceDue(g, 5), 0);
    inv(g);
    gateFree(g);
}

/* ------------------------------- A tiny ALPHA ----------------------------
 * A candidate may consume, for each tested label, 10 times the correct
 * answers a flawless threshold needs. For a tiny alpha a correct answer
 * multiplies a bet by 1 + c alpha / (1 - alpha), that is exp(c alpha) for
 * all a double can tell, so n of them reach the bar 640 when
 *
 *     (e^0.1X + e^0.25X + e^0.5X + e^0.9X) / 4 = 640,  X = n alpha:
 *
 * X = 8.680921243 (bisection, 60 digits, in the scratch script), and the
 * budget of two labels is 20 X / alpha. For an alpha of 1e-18 that is more
 * than a counter may hold (2^62 = 4.6e18): the budget saturates there (such
 * a snapshot never runs out) instead of wrapping around to a small number. */
TEST(tiny_alpha) {
    static const struct {
        double alpha;
        int saturates;
    } cases[] = {
        {1e-9, 0},                   /* 1.74e11 answers */
        {1e-15, 0},                  /* 1.74e17 */
        {1e-18, 1},                  /* 1.74e20: more than 2^62 */
        {1e-100, 1},
        {1e-300, 1},
        {4.9406564584124654e-324, 1} /* the smallest double there is */
    };
    gatePredictResult r;
    gateConfig cfg;
    int64_t now = T0;
    size_t c;
    uint32_t i;
    gate *g;

    for (c = 0; c < COUNTOF(cases); c++) {
        const double alpha = cases[c].alpha;
        uint64_t budget;

        CHECK(alpha > 0.0);
        classConfig(&cfg, 2, 3);
        cfg.alpha = alpha;
        cfg.class_alpha[0] = cfg.class_alpha[1] = 0.0; /* "not set": the gate's rate */
        g = create(&cfg);
        CHECK(gateRowAlpha(g, 0) == alpha && gateRowAlpha(g, 1) == alpha);
        for (i = 0; i < 3; i++) train(g, now, &XA, 0);
        CHECK(g->candidate != NULL);
        CHECK_NEAR(candidateOf(g)->log_threshold, log(640.0), 1e-12);
        budget = candidateOf(g)->budget;
        if (cases[c].saturates) {
            CHECK(budget == GATE_COUNT_MAX);
        } else {
            /* 20 X / alpha, give or take the rounding of a number of 17
             * digits and the terms of order alpha that were left out. */
            double expect = 20.0 * 8.680921243071954 / alpha;

            CHECK(budget < GATE_COUNT_MAX && budget > 0);
            CHECK_NEAR((double)budget / expect, 1.0, 1e-8);
            CHECK_EQ_INT(budget % 20, 0); /* a whole number of answers, 10 times, for two labels */
        }
        /* Such a gate works; it just never proves anything in a lifetime,
         * and its snapshot is not dropped for it. */
        for (i = 0; i < 200; i++) {
            r = askText(g, now, U_PLAIN, &XA);
            CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0);
            CHECK_EQ_INT(answer(g, now, r.seq, (int32_t)(i % 5 == 0)), 1);
        }
        CHECK(g->candidate != NULL && candidateOf(g)->gen == 1);
        CHECK_EQ_INT(candidateOf(g)->samples, 200);
        CHECK_EQ_INT(candidateOf(g)->errors, 40);
        CHECK_EQ_INT(candidateOf(g)->policy[0], -1);
        CHECK(isfinite(gateRegionLogWealth(&candidateOf(g)->regions[0])));
        CHECK(gateRegionLogWealth(&candidateOf(g)->regions[0]) < 0.0);
        CHECK_EQ_INT(g->stats.retired, 0);
        gateFree(g);
    }

    /* One strict label is enough: the budget is that of the most demanding
     * tested label. A label that is never answered demands nothing. */
    classConfig(&cfg, 3, 3);
    cfg.class_alpha[1] = 1e-30;
    g = create(&cfg);
    for (i = 0; i < 3; i++) train(g, now, &XA, 0);
    CHECK(candidateOf(g)->budget == GATE_COUNT_MAX);
    inv(g);
    gateFree(g);
    classConfig(&cfg, 3, 3);
    cfg.class_alpha[1] = 1e-30;
    cfg.class_defer[1] = 1;
    g = create(&cfg);
    for (i = 0; i < 3; i++) train(g, now, &XA, 0);
    CHECK_EQ_INT(candidateOf(g)->budget, 2 * 10 * 39);
    gateFree(g);

    /* A score gate has no budget whatever its alpha. */
    baseConfig(&cfg);
    cfg.alpha = 1e-300;
    g = create(&cfg);
    CHECK_EQ_INT(candidateOf(g)->budget, 0);
    for (i = 0; i < 50; i++) feed(g, now, U_PLAIN, 0.9, 1);
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    CHECK_EQ_INT(candidateOf(g)->samples, 50);
    gateFree(g);
}

TEST(memory_usage) {
    static const char *const labels[] = {"a", "four", "a-much-longer-label"};
    static const size_t lens[] = {1, 4, 19};
    gateConfig cfg;
    gatePredictResult r;
    int64_t now = T0;
    uint64_t base;
    gate *g;
    uint32_t i;

    /* Score gate: the struct, one row of increments, the index, and one
     * generation of 32 regions, each with its paired count. */
    g = scoreGate();
    base = sizeof(gate) + sizeof(gateBets) + sizeof(gateDriftBets) +
           GATE_TABLE_SIZE * sizeof(gateObs *);
    CHECK_EQ_INT(expectBytes(g), base + sizeof(gateGeneration) +
                                     T * (sizeof(gateRegion) + sizeof(uint64_t)) +
                                     sizeof(int32_t) + sizeof(gateDrift));
    CHECK_EQ_INT(gateMemoryUsage(g), expectBytes(g));
    CHECK_EQ_INT(gateFreeEffort(g), 2);
    for (i = 0; i < 10; i++) ask(g, now, U_PLAIN, 0.9);
    CHECK_EQ_INT(gateMemoryUsage(g), expectBytes(g));
    CHECK_EQ_INT(gateMemoryUsage(g) - base - sizeof(gateGeneration) -
                 T * (sizeof(gateRegion) + sizeof(uint64_t)) -
                 sizeof(int32_t) - sizeof(gateDrift), 10 * sizeof(gateObs));
    CHECK_EQ_INT(gateFreeEffort(g), 12);
    gateFree(g);

    /* Classifier: labels, three weight vectors, later two frozen copies. */
    baseConfig(&cfg);
    cfg.kind = GATE_KIND_CLASSIFIER;
    cfg.snapshot_examples = 6;
    if (gateConfigSetClasses(&cfg, labels, lens, 3) != GATE_OK) die("out of memory");
    g = create(&cfg);
    CHECK_EQ_INT(gateMemoryUsage(g), expectBytes(g));
    CHECK_EQ_INT(gateMemoryUsage(g),
                 sizeof(gate) + 3 * (sizeof(char *) + sizeof(double) + 1) + (2 + 5 + 20) +
                 3 * (sizeof(gateBets) + sizeof(gateDriftBets)) +
                 GATE_TABLE_SIZE * sizeof(gateObs *) + 3 * 8193 * sizeof(float));
    CHECK_EQ_INT(gateFreeEffort(g), 1);
    r = askText(g, now, U_PLAIN, &XA);
    CHECK_EQ_INT(gateMemoryUsage(g), expectBytes(g));
    for (i = 0; i < 6; i++) train(g, now, &XA, 0);
    CHECK(g->candidate != NULL);
    CHECK_EQ_INT(gateMemoryUsage(g), expectBytes(g));
    for (i = 0; i < 41; i++) feedText(g, now, U_PLAIN, &XA, 0); /* three labels: 41 to prove */
    CHECK(g->serving != NULL && g->candidate != NULL);
    CHECK_EQ_INT(gateMemoryUsage(g), expectBytes(g));
    CHECK_EQ_INT(gateFreeEffort(g), 4);
    CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
    CHECK_EQ_INT(gateMemoryUsage(g), expectBytes(g));
    gateFree(g);

    g = classGate(2, 6); /* two labels share one weight vector */
    CHECK_EQ_INT(gateMemoryUsage(g), expectBytes(g));
    gateFree(g);
}

/* gateFree() with every kind of waiting request: the leak check of the
 * runner (and of valgrind) is the expectation. */
TEST(free_with_waiting_requests) {
    gate *g = classGate(2, 12);
    gatePredictResult r;
    int64_t now = T0;
    uint32_t i;

    for (i = 0; i < 50; i++) askText(g, now, U_PLAIN, &XA); /* training only */
    for (i = 0; i < 12; i++) train(g, now, (i & 1) ? &XB : &XA, (int32_t)(i & 1));
    for (i = 0; i < 39; i++) feedText(g, now, U_PLAIN, &XA, 0);
    for (i = 0; i < 50; i++) askText(g, now, U_SPOT, &XA);  /* spot checks */
    r = askText(g, now, U_PLAIN, &XB);                      /* proof, answered */
    CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
    CHECK(flagsOf(g, r.seq) & GATE_OBS_RESOLVED);
    CHECK(g->serving != NULL && g->candidate != NULL);
    CHECK_EQ_INT(g->npending, 101);
    gateFree(g);
    gateFree(NULL);

    /* ...and with requests that proof has moved on without, which are in
     * no queue: with their clues (classifier) and without. */
    g = trainedGate(12);
    for (i = 0; i < 20; i++) askText(g, now, U_PLAIN, &XA);
    r = askText(g, now, U_PLAIN, &XA);
    CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
    gateMaintain(g, now + GRACE);
    inv(g);
    CHECK_EQ_INT(g->npending, 20);
    CHECK_EQ_INT(g->nevidence, 0);
    CHECK_EQ_INT(g->pending_bytes, 20 * GATE_OBS_BYTES(1));
    gateFree(g);
    g = scoreGate();
    for (i = 0; i < 20; i++) ask(g, now, U_PLAIN, 0.9);
    r = ask(g, now, U_PLAIN, 0.9);
    CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
    gateMaintain(g, now + GRACE);
    inv(g);
    CHECK_EQ_INT(g->npending, 20);
    CHECK_EQ_INT(g->pending_bytes, 20 * GATE_OBS_BYTES(0));
    gateFree(g);
}

/* What the codec builds a gate from. */
TEST(constructors) {
    gateConfig cfg;
    gateGeneration *gen;
    gateObs *o[3];
    gate *g;
    int i;

    classConfig(&cfg, 2, 12);
    if (gateConfigValidate(&cfg, NULL, 0) != GATE_OK) die("invalid config");
    g = gateAllocate(&cfg);
    if (g == NULL) die("out of memory");
    gateConfigFree(&cfg);
    CHECK(gateState(g) == GATE_STATE_COLD);
    CHECK(g->candidate == NULL && g->serving == NULL && g->oldest == NULL);
    CHECK(g->next_seq == 1 && g->generations == 0 && g->npending == 0 && g->epoch == 0 &&
          g->lineage == 0 && g->now_ms == 0 && g->paused == 0 && g->runtime_tag == 0 &&
          g->runtime_clock == 0);
    CHECK(g->trainer.w != NULL && g->trainer.updates == 0 && g->table != NULL);
    CHECK(g->grid[0] == 0.5 && g->bets[1].alpha == ALPHA);
    CHECK_EQ_STR(g->cfg.labels[0], "label0");
    inv(g);

    gen = gateGenerationAllocate(g);
    if (gen == NULL) die("out of memory");
    gen->gen = 3;
    gen->log_threshold = barOf(3, 2, DELTA);
    if (gateModelCopy(&gen->model, &g->trainer) != GATE_OK) die("out of memory");
    g->generations = 3;
    g->candidate = gen;

    for (i = 0; i < 3; i++) {
        o[i] = gateObsAllocate();
        if (o[i] == NULL) die("out of memory");
        CHECK(o[i]->result == -1 && o[i]->cand_label == -1 && o[i]->serv_label == -1 &&
              o[i]->seq == 0 && o[i]->flags == 0 && o[i]->x.n == 0 && o[i]->x.idx == NULL &&
              o[i]->cand_gen == 0 && o[i]->serv_gen == 0 && o[i]->next == NULL);
    }
    o[0]->seq = 5; /* proof for the candidate */
    o[0]->flags = GATE_OBS_CAND;
    o[0]->cand_gen = 3;
    o[0]->cand_label = 0;
    o[0]->cand_score = 0.9;
    o[1]->seq = 9; /* training only, with its clues */
    if (gateSparseCopy(&o[1]->x, &XA) != GATE_OK) die("out of memory");
    o[2]->seq = 12;
    o[2]->flags = GATE_OBS_CAND | GATE_OBS_AUDIT;
    o[2]->cand_gen = 3;
    o[2]->cand_label = 1;
    o[2]->cand_score = 0.7;
    g->next_seq = 13;
    for (i = 0; i < 3; i++) gateObsLink(g, o[i]);

    CHECK_EQ_INT(g->npending, 3);
    CHECK_EQ_INT(g->nevidence, 2);
    CHECK_EQ_INT(g->pending_bytes, 3 * sizeof(gateObs) + sizeof(uint32_t) + sizeof(float));
    CHECK(g->oldest == o[0] && g->newest == o[2] && o[0]->next == o[1] && o[2]->prev == o[1]);
    CHECK(g->ehead == o[0] && g->etail == o[2] && o[0]->enext == o[2]);
    CHECK(gateFindObservation(g, 5) == o[0] && gateFindObservation(g, 9) == o[1] &&
          gateFindObservation(g, 12) == o[2]);
    CHECK(gateFindObservation(g, 6) == NULL && gateFindObservation(g, 0) == NULL);
    inv(g);
    CHECK_EQ_INT(gateMemoryUsage(g), expectBytes(g));

    /* And it is a working gate: the answers find their requests. */
    CHECK_EQ_INT(gateLearn(g, 10, 0, 12, 1), 1);
    CHECK_EQ_INT(candidateOf(g)->samples, 0);
    CHECK_EQ_INT(gateLearn(g, 10, 0, 5, 1), 1);
    CHECK_EQ_INT(candidateOf(g)->samples, 2);
    CHECK_EQ_INT(candidateOf(g)->errors, 1);
    CHECK_EQ_INT(gateLearn(g, 10, 0, 9, 0), 1);
    CHECK_EQ_INT(g->npending, 0);
    CHECK_EQ_INT(g->trainer.updates, 3);
    inv(g);
    gateFree(g);

    /* Pieces that were never attached are released by themselves. */
    classConfig(&cfg, 2, 12);
    g = create(&cfg);
    gen = gateGenerationAllocate(g);
    o[0] = gateObsAllocate();
    if (gen == NULL || o[0] == NULL || gateSparseCopy(&o[0]->x, &XB) != GATE_OK)
        die("out of memory");
    gateObsRelease(o[0]);
    gateGenerationRelease(gen);
    gateObsRelease(NULL);
    gateGenerationRelease(NULL);
    gateFree(g);
}

/* --------------------------- Allocation failures ------------------------- */

TEST(allocation_failures) {
    gateConfig cfg, copy;
    gatePredictResult r;
    gateSparse x;
    gateEval ev;
    gate *g;
    long k;
    int kind, injected = 1;

    /* gateCreate(): let each of its allocations fail in turn. */
    for (kind = 0; kind < 2; kind++) {
        for (k = 0; k < 100 && injected; k++) {
            if (kind == 0) classConfig(&cfg, 3, 10); else baseConfig(&cfg);
            if (gateConfigValidate(&cfg, NULL, 0) != GATE_OK) die("invalid config");
            fail_in = k;
            g = gateCreate(&cfg, LINEAGE, EPOCH, T0);
            injected = fail_in == -1;
            fail_in = -1;
            if (g == NULL) {
                CHECK(injected);
            } else if (injected) {
                /* The gate itself was built; its first snapshot was not,
                 * and is taken by the next call without using a number. */
                CHECK(kind == 1 && g->candidate == NULL && g->generations == 0);
                inv(g);
                CHECK_EQ_INT(gateMaintenanceDue(g, T0), 1);
                gateMaintain(g, T0);
                CHECK(g->candidate != NULL && candidateOf(g)->gen == 1);
            }
            if (g) inv(g);
            gateFree(g);
            gateConfigFree(&cfg);
            CHECK_EQ_INT(live_blocks, 0);
        }
        CHECK(k > 5 && k < 100); /* several failures were tried, then none */
        injected = 1;
    }

    /* gateConfigCopy(): all or nothing. */
    classConfig(&cfg, 3, 10);
    for (k = 0; k < 100 && injected; k++) {
        int rc;

        fail_in = k;
        rc = gateConfigCopy(&copy, &cfg);
        injected = fail_in == -1;
        fail_in = -1;
        CHECK_EQ_INT(rc == GATE_ERR, injected);
        if (rc == GATE_ERR) CHECK(copy.labels == NULL && copy.nclasses == 0);
        gateConfigFree(&copy);
    }
    CHECK(k > 3 && k < 100);
    gateConfigFree(&cfg);
    CHECK_EQ_INT(live_blocks, 0);

    /* A request that cannot be recorded gets no id, and keeps its clues. */
    g = classGate(2, 3);
    CHECK(gateEvaluate(g, &XA, 0.0, &ev) == GATE_OK);
    if (gateSparseCopy(&x, &XA) != GATE_OK) die("out of memory");
    fail_in = 0;
    fail_forever = 1;
    CHECK(gatePredict(g, T0, U_PLAIN, &ev, &x, &r) == GATE_OK);
    CHECK(r.seq == 0 && r.action == GATE_ACTION_ABSTAIN);
    CHECK(x.n == 1 && x.idx != NULL);
    CHECK(g->npending == 0 && g->next_seq == 1 && g->stats.observations == 0);
    inv(g);
    /* A snapshot that cannot be taken is taken later; meanwhile its
     * number stays unused. */
    for (k = 0; k < 3; k++) CHECK(gateTrain(g, T0, &XA, 0) == GATE_OK);
    CHECK(g->candidate == NULL && g->generations == 0);
    CHECK_EQ_INT(gateMaintenanceDue(g, T0), 1);
    inv(g);
    fail_in = -1;
    fail_forever = 0;
    gateMaintain(g, T0);
    CHECK(g->candidate != NULL && candidateOf(g)->gen == 1 && g->generations == 1);
    CHECK_EQ_INT(gateMaintenanceDue(g, T0), 0);
    inv(g);
    gateSparseFree(&x);
    gateFree(g);

    /* An answer the gate would have spot checked: without a record there is
     * no id to answer, so the reply is an ABSTAIN without one. Never an
     * AUDIT, which promises an id. */
    for (kind = 0; kind < 2; kind++) {
        gateCounters st;
        uint64_t next_seq;
        uint32_t i;

        if (kind == 0) {
            g = activeScoreGate(T0);
        } else {
            g = trainedGate(12);
            for (i = 0; i < 39; i++) feedText(g, T0, U_PLAIN, &XA, 0);
            CHECK(g->serving != NULL);
        }
        st = g->stats;
        next_seq = g->next_seq;
        gateMaintain(g, T0);
        CHECK(gateEvaluate(g, kind ? &XA : NULL, 0.9, &ev) == GATE_OK);
        if (gateSparseCopy(&x, &XA) != GATE_OK) die("out of memory");
        fail_in = 0;
        CHECK(gatePredict(g, T0, U_SPOT, &ev, kind ? &x : NULL, &r) == GATE_OK);
        CHECK_EQ_INT(fail_in, -1); /* the failure was injected */
        CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq == 0 && r.label == -1);
        CHECK(x.n == 1 && x.idx != NULL); /* the clues are still the caller's */
        CHECK_EQ_INT(g->stats.requests, st.requests + 1);
        CHECK_EQ_INT(g->stats.abstained, st.abstained + 1);
        CHECK_EQ_INT(g->stats.audited, st.audited);
        CHECK_EQ_INT(g->stats.accepted, st.accepted);
        CHECK_EQ_INT(g->stats.observations, st.observations);
        CHECK_EQ_INT(g->stats.full, 0);
        CHECK_EQ_INT(g->next_seq, next_seq);
        CHECK_EQ_INT(g->npending, 0);
        inv(g);
        /* With memory again the same request is the spot check it is. */
        CHECK(gatePredict(g, T0, U_SPOT, &ev, kind ? &x : NULL, &r) == GATE_OK);
        CHECK(r.action == GATE_ACTION_AUDIT && r.seq == next_seq);
        CHECK_EQ_INT(g->stats.audited, st.audited + 1);
        if (kind) CHECK(x.n == 0 && x.idx == NULL);
        inv(g);
        gateSparseFree(&x);
        gateFree(g);
    }
}

/* ----------------------------- A literal model ----------------------------
 * The rules in the comment at the top of gate_core.h, written a second time
 * the slow and obvious way: one array of waiting requests in request order,
 * every question answered by a linear scan, generations held by value. It
 * shares with the gate only the betting arithmetic of gate_stats (tested on
 * its own) and the threshold grid (checked in "grids" above). The model
 * does not have the classifier's weights: guesses are inputs to both, and
 * of the live model it only counts the examples.
 *
 * Three things the gate keeps as counters the model keeps as lists, and
 * answers by counting: what the serving generation guessed for each proof
 * request of the candidate (the gate: the paired counts), what became of
 * each spot check of each row in the order they were asked (the gate: the
 * drift detectors; the model has no detector, it multiplies the block out
 * with driftStat() every time), and the proof a flawless threshold needs
 * (the gate: gateBetsFlawless(); the model: needCorrect()).
 *
 * It also knows what a gate cannot: the answer the teacher has for every
 * request, whether it arrives or not. Next to the regions of a generation
 * it keeps the regions those answers give ("truth"): the same requests in
 * the same order, a wrong guess only where the guess was wrong. The wealth
 * of the gate must never be above that one, and a threshold may be proven
 * only if that wealth has been at the bar: the argument that makes "count
 * it as wrong for now" safe, checked on every region after every step. */

#define MODEL_ROWS 3
#define MODEL_REQS (GATE_MAX_PENDING + GATE_MAX_WAITING + 64) /* more cannot wait */
#define MODEL_SLACK 1e-9 /* rounding allowed between the two wealths */

/* What became of a spot check, as far as the drift detector is concerned:
 * nothing yet or an answer that agreed (all the same to it), an answer that
 * disagreed, no answer for good. */
enum { SPOT_FINE, SPOT_WRONG, SPOT_LOST };

typedef struct modelGen {
    uint64_t gen; /* 0: there is no such generation */
    double bar;
    gateRegion reg[MODEL_ROWS][T];
    gateRegion truth[MODEL_ROWS][T]; /* with the teacher's answers */
    int reached[MODEL_ROWS][T];      /* ... that wealth has been at the bar */
    int32_t policy[MODEL_ROWS];
    uint64_t samples, errors, audits, audit_errors, missed, budget;
    uint64_t asked[MODEL_ROWS];      /* spot checks numbered while it serves */
    uint64_t disagreed[MODEL_ROWS];  /* ... whose answer disagreed */
    uint64_t would;                  /* candidate: proof requests the serving
                                      * generation would have answered when
                                      * they were asked (the rule that was) */
} modelGen;

typedef struct modelReq {
    uint64_t seq, cand_gen, serv_gen;
    int64_t created;
    int audit, would;   /* the draw; the serving generation would have answered */
    int cand, serv;     /* proof for generation cand_gen / serv_gen */
    int answered;       /* the answer has arrived and waits for its turn */
    int provisional;    /* proof has moved on: a wrong guess for now */
    int32_t cand_label, serv_label, result;
    int32_t truth;      /* what the teacher says, whether it arrives or not */
    double cand_score, serv_score;
    uint64_t spot;      /* its number as a spot check of its row; 0: it is none */
    uint32_t nnz;       /* clues still stored with it */
} modelReq;

/* Where the serving generation put a proof request of the candidate. */
typedef struct modelPair {
    int32_t label;
    double score;
} modelPair;

typedef struct model {
    const gateConfig *cfg;
    int classifier;
    uint32_t rows;
    double grid[T];
    gateBets bets[MODEL_ROWS];
    int64_t now;
    uint64_t epoch, generations, next_seq;
    uint64_t updates, snapshot_updates; /* examples the live model learned */
    int paused, blind;
    uint32_t missing_run;
    uint64_t answer_mark; /* the next request number when an answer was last used */
    modelGen cand, serv;
    modelReq *base, *req; /* waiting requests, oldest first: req[0..n-1] */
    uint32_t n, answered; /* ... of which answered */
    uint64_t nnz;         /* clues stored with them, in all */
    modelPair *pairs;     /* the candidate's proof requests, see modelPair */
    uint32_t npairs, pairs_cap;
    uint8_t *spots[MODEL_ROWS]; /* the serving generation's spot checks: SPOT_* */
    uint64_t spots_cap[MODEL_ROWS];
    gateCounters st;
    /* Where the walk went. */
    uint64_t passes, limit_passes, taken_back, late_answers, late_missing, blinds;
    uint64_t lost_answers; /* missing answers that were no part of a run */
    uint64_t worse, unchecked, unchecked_by_answer, late_reports, numbered;
    uint64_t missed, missed_out; /* requests counted as missed; snapshots dropped with some */
    uint64_t held_back;          /* takeovers the rule that was would have made */
} model;

static int modelTested(const model *m, uint32_t row) {
    return !m->classifier || !m->cfg->class_defer[row];
}

static double modelAlpha(const model *m, uint32_t row) {
    return m->classifier ? m->cfg->class_alpha[row] : m->cfg->alpha;
}

/* How long proof waits for a late answer: a quarter of the timeout. */
static int64_t modelGrace(const model *m) {
    int64_t grace = m->cfg->feedback_timeout_ms / 4;

    return grace > 1 ? grace : 1;
}

static void modelSnapshot(model *m) {
    modelGen *c = &m->cand;
    uint32_t r, t, tested = 0;
    uint64_t need = 0;

    memset(c, 0, sizeof(*c));
    c->gen = ++m->generations;
    for (r = 0; r < m->rows; r++) {
        tested += (uint32_t)modelTested(m, r);
        c->policy[r] = -1;
        for (t = 0; t < T; t++) {
            gateRegionInit(&c->reg[r][t]);
            gateRegionInit(&c->truth[r][t]);
        }
    }
    c->bar = barOf(c->gen, tested, m->cfg->delta);
    /* A classifier snapshot may use, for each tested label, 10 times the
     * correct answers in a row that take a threshold of its most demanding
     * label to the bar. */
    for (r = 0; m->classifier && r < m->rows; r++) {
        uint64_t n = modelTested(m, r) ? needCorrect(modelAlpha(m, r), c->bar, 0) : 0;

        if (n > need) need = n;
    }
    c->budget = need * 10 * tested;
    m->npairs = 0;
    m->snapshot_updates = m->updates;
}

static int modelSnapshotDue(const model *m) {
    if (m->cand.gen || m->blind) return 0;
    if (m->classifier) return m->updates - m->snapshot_updates >= m->cfg->snapshot_examples;
    return m->serv.gen == 0;
}

/* The lowest proven threshold of a row. */
static int32_t modelPolicy(const modelGen *gen, uint32_t row) {
    uint32_t t, mask = 0;

    for (t = 0; t < T; t++) mask |= (uint32_t)(gen->reg[row][t].certified_at != 0) << t;
    return policyByDefinition(mask);
}

/* One answer for the guessed row of a generation: 'error' is what the
 * proof is told, 'truly' what the teacher says. (A guess of a label that
 * is never answered is proof for nothing; such a request is not recorded
 * as proof in the first place.) */
static void modelFeed(model *m, modelGen *gen, int32_t label, double score, int error, int truly) {
    uint32_t t, row = m->classifier ? (uint32_t)label : 0;

    if (!modelTested(m, row)) return;
    gen->samples++;
    gen->errors += (uint64_t)error;
    for (t = 0; t < T; t++) {
        gateRegion *reg = &gen->reg[row][t], *truth = &gen->truth[row][t];

        if (!(m->grid[t] <= score)) continue;
        gateRegionUpdate(reg, &m->bets[row], error);
        if (reg->certified_at == 0 && gateRegionCertifies(reg, gen->bar))
            reg->certified_at = reg->n;
        gateRegionUpdate(truth, &m->bets[row], truly);
        if (gateRegionLogWealth(truth) >= gen->bar - 2 * MODEL_SLACK) gen->reached[row][t] = 1;
    }
    gen->policy[row] = modelPolicy(gen, row);
}

/* A guess that was counted as wrong turned out to be right. */
static void modelTakeBack(model *m, modelGen *gen, int32_t label, double score) {
    uint32_t t, row = m->classifier ? (uint32_t)label : 0;

    if (!modelTested(m, row)) return;
    gen->errors--;
    for (t = 0; t < T; t++) {
        gateRegion *reg = &gen->reg[row][t];

        if (!(m->grid[t] <= score)) continue;
        gateRegionFlip(reg, &m->bets[row]);
        if (reg->certified_at == 0 && gateRegionCertifies(reg, gen->bar))
            reg->certified_at = reg->n;
    }
    gen->policy[row] = modelPolicy(gen, row);
}

static int modelAnswers(const model *m, const modelGen *gen) {
    uint32_t r;

    for (r = 0; r < m->rows; r++)
        if (gen->policy[r] >= 0) return 1;
    return 0;
}

/* Would a generation answer a guess, with the policy it has now? */
static int modelInPolicy(const model *m, const modelGen *gen, int32_t label, double score) {
    uint32_t row = m->classifier ? (uint32_t)label : 0;
    int32_t t = gen->policy[row];

    return modelTested(m, row) && t >= 0 && score >= m->grid[t];
}

/* Proof requests of a generation that its policy would answer. */
static uint64_t modelInside(const model *m, const modelGen *gen) {
    uint64_t n = 0;
    uint32_t r;

    for (r = 0; r < m->rows; r++)
        if (gen->policy[r] >= 0) n += gen->reg[r][gen->policy[r]].n;
    return n;
}

/* Of the candidate's proof requests, those the serving generation would
 * answer with the policy it has now: each judged by the guess the serving
 * generation recorded for it. */
static uint64_t modelPaired(const model *m) {
    uint64_t n = 0;
    uint32_t i;

    for (i = 0; i < m->npairs; i++)
        n += (uint64_t)modelInPolicy(m, &m->serv, m->pairs[i].label, m->pairs[i].score);
    return n;
}

static void modelServingGone(model *m) {
    m->st.window = 0;
    m->st.coverage = 0.0;
}

/* The candidate takes over if it has earned it, or goes at its budget. */
static void modelConsider(model *m) {
    modelGen *c = &m->cand;

    if (modelAnswers(m, c) && (m->serv.gen == 0 || modelInside(m, c) > modelPaired(m))) {
        m->serv = *c; /* with no spot check numbered yet */
        m->serv.budget = 0;
        m->serv.would = 0;
        m->npairs = 0;
        c->gen = 0;
        m->st.promotions++;
        modelServingGone(m);
        return;
    }
    /* What the rule was before: it counted what the serving generation
     * would have answered when each request was asked. */
    if (modelAnswers(m, c) && modelInside(m, c) > c->would) m->held_back++;
    if (c->budget && c->samples + c->missed >= c->budget) {
        m->missed_out += c->missed != 0;
        c->gen = 0;
        m->npairs = 0;
        m->st.retired++;
    }
}

/* Whether each recorded guess is wrong by the answer 'result'. */
static void modelErrors(const model *m, const modelReq *q, int32_t result, int *cand, int *serv) {
    if (m->classifier) {
        *cand = q->cand_label != result;
        *serv = q->serv_label != result;
    } else {
        *cand = *serv = !result;
    }
}

/* Count a request as proof, in its turn, for the generations it is proof
 * for and that are still there. */
static void modelCount(model *m, const modelReq *q, int err_cand, int err_serv) {
    int true_cand, true_serv;

    modelErrors(m, q, q->truth, &true_cand, &true_serv);
    /* For the serving generation first: the candidate is compared with the
     * policy the serving one has now, this request included. */
    if (q->serv && m->serv.gen == q->serv_gen)
        modelFeed(m, &m->serv, q->serv_label, q->serv_score, err_serv, true_serv);
    if (q->cand && m->cand.gen == q->cand_gen) {
        modelFeed(m, &m->cand, q->cand_label, q->cand_score, err_cand, true_cand);
        /* One more proof request of the candidate. If a generation served
         * when it was asked, and still does: what did that one guess? */
        if (m->serv.gen && m->serv.gen == q->serv_gen) {
            if (m->npairs == m->pairs_cap) {
                m->pairs_cap = m->pairs_cap ? 2 * m->pairs_cap : 1024;
                m->pairs = realloc(m->pairs, sizeof(modelPair) * m->pairs_cap);
                if (m->pairs == NULL) die("out of memory");
            }
            m->pairs[m->npairs].label = q->serv_label;
            m->pairs[m->npairs++].score = q->serv_score;
            m->cand.would += (uint64_t)q->would;
        }
        modelConsider(m);
    } else if (q->cand && m->serv.gen == q->cand_gen) {
        /* Asked of the candidate, which serves by now. */
        modelFeed(m, &m->serv, q->cand_label, q->cand_score, err_cand, true_cand);
    }
}

/* The answer of a request that was counted as a wrong guess has arrived:
 * the same generations take it back where their guess was right. */
static void modelUncount(model *m, const modelReq *q, int err_cand, int err_serv) {
    /* The serving generation first, as when the request was counted. */
    if (q->serv && m->serv.gen == q->serv_gen && !err_serv) {
        modelTakeBack(m, &m->serv, q->serv_label, q->serv_score);
        m->taken_back++;
    }
    if (q->cand && m->cand.gen == q->cand_gen) {
        if (!err_cand) {
            modelTakeBack(m, &m->cand, q->cand_label, q->cand_score);
            m->taken_back++;
            modelConsider(m);
        }
    } else if (q->cand && m->serv.gen == q->cand_gen) {
        if (!err_cand) {
            modelTakeBack(m, &m->serv, q->cand_label, q->cand_score);
            m->taken_back++;
        }
    }
}

/* The drift detector says "worse": everything goes, the model included. */
static void modelDrift(model *m) {
    m->serv.gen = m->cand.gen = 0;
    m->npairs = 0;
    if (m->classifier) m->updates = 0;
    m->snapshot_updates = 0;
    m->st.revocations++;
    m->missing_run = 0;
    m->worse++;
    modelServingGone(m);
}

/* The gate cannot check itself (32 answers missing in a row, or lost spot
 * checks that may hide anything): the proof goes, the model stays, and no
 * snapshot is taken until an answer arrives. */
static void modelBlind(model *m) {
    if (m->serv.gen) m->st.revocations++;
    m->serv.gen = m->cand.gen = 0;
    m->npairs = 0;
    m->snapshot_updates = 0;
    m->blind = 1;
    m->missing_run = 0;
    m->blinds++;
    modelServingGone(m);
}

/* A spot check of the serving generation is asked: the next number of its
 * row. Nothing has become of it yet. */
static uint64_t modelSpotAsk(model *m, uint32_t row) {
    uint64_t n = m->serv.asked[row];

    if (n == m->spots_cap[row]) {
        m->spots_cap[row] = n ? 2 * n : 256;
        m->spots[row] = realloc(m->spots[row], m->spots_cap[row]);
        if (m->spots[row] == NULL) die("out of memory");
    }
    m->spots[row][n] = SPOT_FINE;
    m->numbered++;
    return ++m->serv.asked[row];
}

/* The spot checks of a row are cut into blocks of 'len' in the order they
 * were asked. Of the block the last one asked is in: how many were asked,
 * and how many count against the gate, with the lost ones or without. */
static void modelBlock(const model *m, uint32_t row, uint64_t len, int with_lost, uint64_t *n,
                       uint64_t *against)
{
    uint64_t asked = m->serv.asked[row], first, i;

    *n = *against = 0;
    if (asked == 0) return;
    first = (asked - 1) / len * len;
    *n = asked - first;
    for (i = first; i < asked; i++) {
        uint8_t what = m->spots[row][i];

        *against += what == SPOT_WRONG || (with_lost && what == SPOT_LOST);
    }
}

/* The answer of spot check 'number' of a row disagreed (SPOT_WRONG) or was
 * given up (SPOT_LOST). What the blocks of 64, 512 and 4096 that it is in
 * make of it, if they are still the open ones: "worse" when one holds
 * disagreements enough for 1000, else "cannot be checked" when one holds
 * disagreements and lost answers enough for 1e9. */
static int modelSpotReport(model *m, uint32_t row, uint64_t number, int what) {
    static const uint64_t lens[3] = {64, 512, 4096};
    const double alpha = modelAlpha(m, row);
    uint64_t asked = m->serv.asked[row], n, against;
    int s, worse = 0, unchecked = 0;

    m->spots[row][number - 1] = (uint8_t)what;
    if (what == SPOT_WRONG) m->serv.disagreed[row]++;
    for (s = 0; s < 3; s++) {
        if ((number - 1) / lens[s] != (asked - 1) / lens[s]) {
            m->late_reports++; /* its block has ended */
            continue;
        }
        if (what == SPOT_WRONG) {
            modelBlock(m, row, lens[s], 0, &n, &against);
            worse |= driftStat(alpha, n, against) >= log(1000.0);
        }
        modelBlock(m, row, lens[s], 1, &n, &against);
        unchecked |= driftStat(alpha, n, against) >= log(1e9);
    }
    return worse ? GATE_DRIFT_ALARM_WORSE
                 : (unchecked ? GATE_DRIFT_ALARM_UNCHECKED : GATE_DRIFT_QUIET);
}

/* What a request ends as. A missing answer, when its time is up; or an
 * answer, which the drift detector hears the moment it arrives if it is a
 * spot check of an answer of the generation that serves. */
static void modelOutcome(model *m, const modelReq *q, int missing, int err_serv) {
    int spot = q->spot != 0 && q->serv && m->serv.gen == q->serv_gen;
    uint32_t row = m->classifier && spot ? (uint32_t)q->serv_label : 0;

    if (missing) {
        m->st.missing++;
        /* Part of an outage only if no answer of any request has arrived
         * since it was asked. Else an answer that got lost. */
        if (q->seq >= m->answer_mark) {
            if (++m->missing_run >= 32) {
                modelBlind(m);
                return;
            }
        } else {
            m->lost_answers++;
        }
        if (spot && modelSpotReport(m, row, q->spot, SPOT_LOST) == GATE_DRIFT_ALARM_UNCHECKED) {
            m->unchecked++;
            modelBlind(m);
        }
        return;
    }
    if (spot) {
        m->serv.audits++;
        if (err_serv) {
            int verdict;

            m->serv.audit_errors++;
            verdict = modelSpotReport(m, row, q->spot, SPOT_WRONG);
            if (verdict == GATE_DRIFT_ALARM_WORSE) {
                modelDrift(m);
            } else if (verdict == GATE_DRIFT_ALARM_UNCHECKED) {
                m->unchecked++;
                m->unchecked_by_answer++;
                modelBlind(m);
            }
        }
    }
}

static void modelRemove(model *m, uint32_t i) {
    m->nnz -= m->req[i].nnz;
    m->answered -= (uint32_t)m->req[i].answered;
    if (i == 0) m->req++; /* the oldest: the array starts one later */
    else memmove(&m->req[i], &m->req[i + 1], sizeof(modelReq) * (m->n - i - 1));
    m->n--;
}

static modelReq *modelAppend(model *m) {
    if (m->req + m->n == m->base + 2 * MODEL_REQS) {
        memmove(m->base, m->req, sizeof(modelReq) * m->n);
        m->req = m->base;
    }
    return &m->req[m->n++];
}

static int modelLive(const model *m, const modelReq *q) {
    if (q->cand && (m->cand.gen == q->cand_gen || m->serv.gen == q->cand_gen)) return 1;
    return q->serv && m->serv.gen == q->serv_gen;
}

/* The head of the queue: the oldest request that is proof and has not been
 * counted. m->n if there is none. */
static uint32_t modelHead(const model *m) {
    uint32_t i = 0;

    while (i < m->n && (m->req[i].provisional || (!m->req[i].cand && !m->req[i].serv))) i++;
    return i;
}

/* Apply answered proof in request order as far as it goes, and take out
 * of the queue what is proof for nothing any more. */
static void modelDrain(model *m) {
    for (;;) {
        uint32_t i = modelHead(m);
        int err_cand, err_serv;
        modelReq q;

        if (i == m->n) return;
        if (!modelLive(m, &m->req[i])) {
            if (m->req[i].answered) {
                modelRemove(m, i);
            } else if (m->classifier) {
                m->req[i].cand = m->req[i].serv = 0; /* its label still trains */
            } else {
                m->st.dropped++;
                modelRemove(m, i);
            }
            continue;
        }
        if (!m->req[i].answered) return;
        q = m->req[i];
        modelRemove(m, i);
        modelErrors(m, &q, q.result, &err_cand, &err_serv);
        modelCount(m, &q, err_cand, err_serv);
    }
}

/* Bring the model to rest: apply what can be applied, let proof move on
 * without answers that are late, take a snapshot if one is due. */
static void modelSettle(model *m) {
    modelDrain(m);
    for (;;) {
        uint32_t i = modelHead(m);

        /* An unanswered request holds up answers; it is a quarter of the
         * timeout old, or more answers wait than may. */
        if (i == m->n || m->answered == 0) break;
        if (m->now - m->req[i].created < modelGrace(m) && m->answered <= GATE_MAX_WAITING) break;
        if (m->now - m->req[i].created < modelGrace(m)) m->limit_passes++;
        m->passes++;
        m->req[i].provisional = 1;
        modelCount(m, &m->req[i], 1, 1);
        modelDrain(m);
    }
    if (modelSnapshotDue(m)) modelSnapshot(m);
}

static void modelMaintain(model *m, int64_t now) {
    if (now > m->now) m->now = now;
    while (m->n && m->now - m->req[0].created >= m->cfg->feedback_timeout_ms) {
        modelReq q = m->req[0];

        modelRemove(m, 0);
        if (q.provisional) {
            /* Counted as a wrong guess already. A missing answer if it is
             * still proof for something. */
            if (modelLive(m, &q)) {
                m->late_missing++;
                modelOutcome(m, &q, 1, 1);
            } else {
                m->st.dropped++;
            }
        } else if (q.cand || q.serv) {
            /* The head of the queue: a wrong guess and a missing answer.
             * (It is proof for something: what is not was taken out of
             * the queue the moment its generations went.) */
            modelCount(m, &q, 1, 1);
            modelOutcome(m, &q, 1, 1);
        } else {
            m->st.dropped++;
        }
        /* Whatever that did to the generations, requests that are proof
         * for nothing do not stay in the queue. */
        modelDrain(m);
    }
    modelSettle(m);
}

/* Is the serving generation going to answer this request by itself? (The
 * model has been maintained.) Nobody needs the candidate's guess then. */
static int modelWouldAnswer(const model *m, double u, const gateEval *ev) {
    if (m->serv.gen == 0 || m->paused || u < m->cfg->audit) return 0;
    return modelInPolicy(m, &m->serv, ev->serving.label, ev->serving.score);
}

static void modelPredict(model *m, int64_t now, double u, const gateEval *ev, uint32_t nnz,
                         int32_t truth, gatePredictResult *res)
{
    int audit = u < m->cfg->audit, would = 0, cand = 0, serv = 0;
    uint32_t unanswered;
    modelReq *q;

    modelMaintain(m, now);
    res->action = GATE_ACTION_ABSTAIN;
    res->label = -1;
    res->seq = 0;
    m->st.requests++;
    /* Full: by the requests that still wait for their answer, or by bytes. */
    unanswered = m->n - m->answered;
    if (unanswered >= GATE_MAX_PENDING ||
        m->n * sizeof(gateObs) + m->nnz * 8 >= GATE_MAX_PENDING_BYTES)
    {
        m->st.abstained++;
        m->st.full++;
        return;
    }
    if (m->serv.gen) {
        uint32_t row = m->classifier ? (uint32_t)ev->serving.label : 0;

        would = modelInPolicy(m, &m->serv, ev->serving.label, ev->serving.score);
        if (modelTested(m, row)) serv = m->serv.policy[row] < 0 || audit;
        if (m->st.window < GATE_COVERAGE_WINDOW) m->st.window++;
        m->st.coverage += ((double)would - m->st.coverage) / (double)m->st.window;
    }
    /* Proof for the candidate: every request while nothing serves, the spot
     * checks afterwards; never a guess of a label that is not answered. */
    if (m->cand.gen && (m->serv.gen == 0 || audit))
        cand = modelTested(m, m->classifier ? (uint32_t)ev->candidate.label : 0);

    if (would && !m->paused && !audit) {
        m->st.accepted++;
        res->action = GATE_ACTION_ACCEPT;
        res->label = ev->serving.label;
        return;
    }
    if (would && !m->paused) {
        m->st.audited++;
        res->action = GATE_ACTION_AUDIT;
    } else {
        m->st.abstained++;
    }
    /* Recorded: proof; or, for training or because the gate is blind and
     * needs an id to be answered by, while fewer than 3072 are unanswered. */
    if (!cand && !serv && !((m->classifier || m->blind) && unanswered < GATE_TRAIN_PENDING))
        return;

    q = modelAppend(m);
    memset(q, 0, sizeof(*q));
    q->seq = m->next_seq++;
    q->created = m->now;
    q->audit = audit;
    q->would = would;
    q->cand = cand;
    q->serv = serv;
    q->cand_label = q->serv_label = q->result = -1;
    q->truth = truth;
    if (m->serv.gen) {
        q->serv_gen = m->serv.gen;
        q->serv_label = ev->serving.label;
        q->serv_score = ev->serving.score;
        /* A spot check of an answer (paused or not): the drift detector of
         * its row numbers it now. */
        if (would && audit) q->spot = modelSpotAsk(m, m->classifier ? (uint32_t)q->serv_label : 0);
    }
    if (m->cand.gen) {
        q->cand_gen = m->cand.gen;
        q->cand_label = ev->candidate.label;
        q->cand_score = ev->candidate.score;
    }
    q->nnz = nnz;
    m->nnz += nnz;
    m->st.observations++;
    res->seq = q->seq;
}

static int modelLearn(model *m, int64_t now, uint64_t epoch, uint64_t seq, int32_t result) {
    int err_cand = 0, err_serv = 0;
    modelReq *q;
    uint32_t i;

    modelMaintain(m, now);
    for (i = 0; i < m->n && m->req[i].seq != seq; i++) {}
    if (epoch != m->epoch || i == m->n || m->req[i].answered) {
        m->st.ignored++;
        return 0;
    }
    q = &m->req[i];
    q->result = result;
    if (m->classifier) m->updates++;
    m->nnz -= q->nnz;
    q->nnz = 0;
    m->st.learned++;
    /* Answers arrive: every request asked so far has seen one. */
    m->blind = 0;
    m->missing_run = 0;
    m->answer_mark = m->next_seq;
    /* A request the candidate could have been tested on (asked of it, at a
     * moment when its requests are proof: nothing serving, or a spot
     * check), had it not guessed a label that is never answered, when the
     * teacher's label is one that is: it uses up the budget like proof. */
    if (m->classifier && m->cand.gen && q->cand_gen == m->cand.gen &&
        (q->serv_gen == 0 || q->audit) && !modelTested(m, (uint32_t)q->cand_label) &&
        modelTested(m, (uint32_t)result))
    {
        m->cand.missed++;
        m->missed++;
        modelConsider(m);
    }
    if (q->cand || q->serv) {
        /* The drift detector hears it now, whatever its place in line. An
         * alarm drops the generations it was proof for. */
        modelErrors(m, q, result, &err_cand, &err_serv);
        if (modelLive(m, q)) modelOutcome(m, q, 0, err_serv);
    }
    if (q->provisional) {
        modelReq late = *q;

        m->late_answers++;
        modelRemove(m, i);
        modelUncount(m, &late, err_cand, err_serv);
    } else if (q->cand || q->serv) {
        q->answered = 1; /* applied when its turn comes */
        m->answered++;
    } else {
        modelRemove(m, i);
    }
    modelSettle(m);
    return 1;
}

static void modelTrain(model *m, int64_t now) {
    modelMaintain(m, now);
    m->updates++;
    modelSettle(m);
}

static void modelPause(model *m, int64_t now, int paused) {
    modelMaintain(m, now);
    m->paused = paused;
}

/* GRESET ('everything') or recovery. Nothing that carries a time is left:
 * the clock is the one they are given. Request numbers go on, and so does
 * the mark that is one of them. */
static void modelForget(model *m, int64_t now, uint64_t epoch, int everything) {
    m->now = now;
    m->req = m->base;
    m->n = m->answered = 0;
    m->nnz = 0;
    m->cand.gen = m->serv.gen = 0;
    m->npairs = 0;
    m->snapshot_updates = 0;
    m->epoch = epoch;
    m->blind = 0;
    m->missing_run = 0;
    modelServingGone(m);
    if (everything) {
        m->updates = 0;
        memset(&m->st, 0, sizeof(m->st));
    }
    modelSettle(m);
}

static void modelInit(model *m, const gateConfig *cfg, const gate *g) {
    uint32_t r;

    memset(m, 0, sizeof(*m));
    m->cfg = cfg;
    m->classifier = cfg->kind == GATE_KIND_CLASSIFIER;
    m->rows = m->classifier ? cfg->nclasses : 1;
    memcpy(m->grid, g->grid, sizeof(m->grid));
    for (r = 0; r < m->rows; r++) gateBetsInit(&m->bets[r], modelAlpha(m, r));
    m->now = g->created_ms;
    m->epoch = g->epoch;
    m->next_seq = 1;
    m->base = calloc(2 * MODEL_REQS, sizeof(modelReq));
    if (m->base == NULL) die("out of memory");
    m->req = m->base;
    modelSettle(m);
}

static void modelFree(model *m) {
    uint32_t r;

    free(m->base);
    free(m->pairs);
    for (r = 0; r < MODEL_ROWS; r++) free(m->spots[r]);
}

#define DIFF(same, what) do { if (!(same)) return what; } while (0)

/* 'serving': the generation is the one that serves. Then its drift
 * detectors are in use, and it has no paired counts; for the candidate it
 * is the other way round. */
static const char *modelDiffGen(const model *m, const modelGen *x, const gateGeneration *gen,
                                int serving)
{
    uint64_t reach[MODEL_ROWS][T + 1];
    uint32_t r, t, i;
    int j, s;

    DIFF((x->gen != 0) == (gen != NULL), "a generation exists in one only");
    if (gen == NULL) return NULL;
    DIFF(gen->gen == x->gen, "generation number");
    DIFF(fabs(gen->log_threshold - x->bar) <= 1e-12, "bar");
    DIFF(gen->samples == x->samples, "samples");
    DIFF(gen->errors == x->errors, "errors");
    DIFF(gen->audits == x->audits, "audits");
    DIFF(gen->audit_errors == x->audit_errors, "audit errors");
    DIFF(gen->missed == x->missed, "missed requests");
    DIFF(gen->budget == x->budget, "budget");
    /* The paired counts by their definition: for each row and threshold,
     * the proof requests of the candidate that the serving generation's
     * guess put in that row at or above that threshold. reach[r][k]:
     * those whose score reaches exactly k thresholds of row r. */
    memset(reach, 0, sizeof(reach));
    for (i = 0; !serving && i < m->npairs; i++) {
        uint32_t row = m->classifier ? (uint32_t)m->pairs[i].label : 0, k = 0;

        if (!modelTested(m, row)) continue;
        for (t = 0; t < T; t++) k += m->grid[t] <= m->pairs[i].score;
        reach[row][k]++;
    }
    for (r = 0; r < m->rows; r++) {
        uint64_t at_or_above = 0;

        for (t = T; t-- > 0;) {
            at_or_above += reach[r][t + 1];
            DIFF(gen->paired[r * T + t] == at_or_above, "paired counts");
        }
    }
    for (r = 0; r < m->rows; r++) {
        const gateDrift *d = &gen->drift[r];

        DIFF(gen->policy[r] == x->policy[r], "policy");
        /* The drift detector against the list of spot checks. */
        DIFF(d->asked == (serving ? x->asked[r] : 0), "spot checks numbered");
        DIFF(d->errors == (serving ? x->disagreed[r] : 0), "spot checks that disagreed");
        for (s = 0; s < GATE_DRIFT_SCALES; s++) {
            uint64_t n = 0, wrong = 0, both = 0;

            if (serving) {
                modelBlock(m, r, gateDriftBlocks[s], 0, &n, &wrong);
                modelBlock(m, r, gateDriftBlocks[s], 1, &n, &both);
            }
            DIFF(gateDriftOpen(d, s) == n, "open block of the drift detector");
            DIFF(d->wrong[s] == wrong, "disagreements of the open block");
            DIFF(d->lost[s] == both - wrong, "lost answers of the open block");
        }
        for (t = 0; t < T; t++) {
            const gateRegion *reg = &gen->regions[r * T + t], *truth = &x->truth[r][t];

            DIFF(memcmp(reg, &x->reg[r][t], sizeof(gateRegion)) == 0, "region");
            /* Against the teacher's answers: the same requests; no fewer
             * wrong guesses; no more wealth, and the same once nothing is
             * counted as wrong that was right; proven only where the true
             * wealth has been at the bar. */
            DIFF(reg->n == truth->n, "proof is not what was asked, in order");
            DIFF(reg->errors >= truth->errors, "fewer wrong guesses than the teacher found");
            for (j = 0; j < GATE_NBETS; j++) {
                DIFF(reg->logw[j] <= truth->logw[j] + MODEL_SLACK, "wealth above the true wealth");
                DIFF(reg->errors != truth->errors ||
                         reg->logw[j] >= truth->logw[j] - MODEL_SLACK,
                     "wealth below the true wealth with no answer outstanding");
            }
            DIFF(reg->certified_at == 0 || x->reached[r][t],
                 "a threshold proven that the true answers do not prove");
        }
    }
    return NULL;
}

/* What differs between the model and the gate, NULL if nothing does.
 * 'requests' also compares the waiting requests one by one. */
static const char *modelDiff(const model *m, const gate *g, int requests) {
    const gateCounters *a = &g->stats, *b = &m->st;
    const char *d;
    const gateObs *o = g->oldest;
    uint32_t i, evidence = 0, answered = 0;

    DIFF(g->now_ms == m->now, "clock");
    DIFF(g->epoch == m->epoch, "epoch");
    DIFF(g->paused == m->paused, "pause");
    DIFF(g->blind == m->blind, "blind");
    DIFF(g->missing_run == m->missing_run, "run of missing answers");
    DIFF(g->answer_mark == m->answer_mark, "answer mark");
    DIFF(g->generations == m->generations, "ledger");
    DIFF(g->next_seq == m->next_seq, "next id");
    DIFF(!m->classifier || g->trainer.updates == m->updates, "examples learned");
    DIFF(g->snapshot_updates == m->snapshot_updates, "examples at the last snapshot");
    if ((d = modelDiffGen(m, &m->cand, g->candidate, 0)) != NULL) return d;
    if ((d = modelDiffGen(m, &m->serv, g->serving, 1)) != NULL) return d;
    DIFF(g->npending == m->n, "number of waiting requests");
    DIFF(g->nanswered == m->answered, "number of answers waiting for their turn");
    DIFF(gateUnanswered(g) == m->n - m->answered, "number of unanswered requests");
    DIFF(g->pending_bytes == m->n * sizeof(gateObs) + m->nnz * 8, "bytes waiting");
    for (i = 0; requests && i < m->n; i++) {
        const modelReq *q = &m->req[i];
        uint32_t flags = (q->audit ? GATE_OBS_AUDIT : 0) | (q->would ? GATE_OBS_WOULD_ACCEPT : 0) |
                         (q->cand ? GATE_OBS_CAND : 0) | (q->serv ? GATE_OBS_SERV : 0) |
                         (q->answered ? GATE_OBS_RESOLVED : 0) |
                         (q->provisional ? GATE_OBS_PROVISIONAL : 0);

        evidence += (q->cand || q->serv) && !q->provisional;
        answered += (uint32_t)q->answered;
        DIFF(o != NULL && o->seq == q->seq, "waiting request");
        DIFF(o->created_ms == q->created, "request time");
        DIFF(o->flags == flags, "request flags");
        DIFF(o->result == q->result, "request answer");
        DIFF(o->cand_gen == q->cand_gen && o->serv_gen == q->serv_gen, "request generations");
        DIFF(o->cand_label == q->cand_label && o->serv_label == q->serv_label, "recorded labels");
        DIFF(o->cand_score == q->cand_score && o->serv_score == q->serv_score, "recorded scores");
        DIFF(o->spot == q->spot, "spot check number");
        DIFF(o->x.n == q->nnz, "stored clues");
        /* Proof moves on only when an answer waits behind a request: a
         * request it moved on without has seen an answer arrive. */
        DIFF(!q->provisional || q->seq < m->answer_mark,
             "proof moved on without a request that has seen no answer arrive");
        o = o->next;
    }
    if (requests) {
        DIFF(g->nevidence == evidence, "proof waiting");
        DIFF(m->answered == answered, "answers waiting, counted");
    }
    DIFF(a->requests == b->requests, "requests");
    DIFF(a->accepted == b->accepted, "accepted");
    DIFF(a->audited == b->audited, "audited");
    DIFF(a->abstained == b->abstained, "abstained");
    DIFF(a->full == b->full, "full");
    DIFF(a->window == b->window, "coverage window");
    DIFF(fabs(a->coverage - b->coverage) <= 1e-9, "coverage");
    DIFF(a->observations == b->observations, "observations");
    DIFF(a->learned == b->learned, "learned");
    DIFF(a->ignored == b->ignored, "ignored");
    DIFF(a->missing == b->missing, "missing");
    DIFF(a->dropped == b->dropped, "dropped");
    DIFF(a->promotions == b->promotions, "promotions");
    DIFF(a->revocations == b->revocations, "revocations");
    DIFF(a->retired == b->retired, "retired");
    return NULL;
}

/* ---------------------------------- Soak ---------------------------------
 * Random operations on three things in step: the gate, the literal model
 * (compared after every operation) and a second gate that is told only
 * what the module tells a replica: a recorded request with the clock, draw
 * and guesses the primary realized (PREDICT), every GLEARN, GADD and
 * administrative command with the clock the primary used, and of all the
 * other requests nothing but a TICK when gateMaintenanceDue() said that
 * maintenance was due. Whatever is replicated must end up equal.
 *
 * The teacher answers like a real one: mostly at once, sometimes out of
 * order, sometimes after proof has moved on, sometimes too late, sometimes
 * never; and now and then not at all for a while (an outage), or for most
 * requests (a partial outage). For a while it holds its answers back and
 * sends them in one batch, in any order; for a while it sends no answer
 * that says "wrong". */

#define SOAK_IDS 32768 /* answers that may be on their way */
#define SOAK_GENS 1024 /* snapshots whose weights are remembered */

typedef struct soakId {
    uint64_t epoch, seq;
    int64_t due;   /* when the answer arrives */
    int32_t truth; /* what the teacher says */
} soakId;

typedef struct soak {
    gateConfig cfg;
    gate *g, *replica;
    model m;
    int classifier;
    int made_up;        /* guesses are drawn, not asked of the frozen models */
    uint64_t rng;
    int64_t now;
    uint32_t op;
    soakId *ids;
    uint32_t nids;
    soakId asked;       /* the request recorded last */
    int asked_wrong;    /* ... the guess of the generation in charge was wrong */
    soakId last;        /* an id that was answered already */
    uint32_t burst;     /* requests left in a burst during which no time passes */
    uint32_t broken;    /* operations left during which guesses are coin flips */
    uint32_t lossy;     /* ... during which one answer in ten gets lost */
    uint32_t slow;      /* ... during which the teacher is slow */
    uint32_t outage;    /* ... during which no answer gets through */
    uint32_t partial;   /* ... during which 19 answers in 20 get lost */
    uint32_t selective; /* ... during which no answer that says "wrong" is sent */
    uint32_t batch;     /* ... during which answers are held back for one batch */
    int64_t batch_due;  /* when that batch arrives */
    uint32_t flush;     /* answers of a batch that arrive now, one after the other */
    int huge;           /* the burst carries the largest clue vectors */
    gateSparse big;
    int failed;
    int was_blind;
    uint64_t frozen[SOAK_GENS]; /* hash of each snapshot's weights, first seen */
    /* Where the walk went. */
    uint64_t accepts, audits, promotions, takeovers, revocations, retired, missing, dropped;
    uint64_t full, full_bytes, untrained, moved_down, resets, recoveries, maintained;
    uint64_t passes, limit_passes, taken_back, late_answers, late_missing, too_late;
    uint64_t blinds, blind_asks, seen_again, outages;
    uint64_t left_out;     /* requests answered without the candidate's guess */
    uint64_t lost_answers, worse, unchecked, unchecked_by_answer, late_reports, numbered;
    uint64_t missed, missed_out, held_back, batches, batched, withheld, resyncs;
} soak;

static uint32_t soakRand(soak *s, uint32_t n) {
    return (uint32_t)(gate_test_rand64(&s->rng) % n);
}

/* The clock a call is given: now and then one that runs behind. */
static int64_t soakWall(soak *s) {
    return soakRand(s, 16) ? s->now : s->now - 1 - soakRand(s, 400);
}

/* The moment the primary really used: what a replica is told. */
static int64_t soakEffective(const soak *s, int64_t wall) {
    return wall > s->g->now_ms ? wall : s->g->now_ms;
}

static void soakFail(soak *s, const char *what, const char *why) {
    if (!s->failed)
        printf("  in %s: operation %u (%s): %s\n", gate_test_current, s->op, what, why);
    s->failed = 1;
}

/* Checks that walk over the waiting requests and their clues are made
 * every time while there is little to walk over, else on a sample. */
static int soakWalk(soak *s) {
    return s->g->pending_bytes <= 32768 || soakRand(s, 32) == 0;
}

#ifdef TEST_CORE_CODEC
/* The replica is thrown away and loaded from the primary's bytes, like a
 * replica that synchronizes anew: from there on it must stay in step. */
static void soakResync(soak *s) {
    unsigned char *buf = NULL;
    size_t len = 0;
    char err[160] = "";
    gate *copy;

    if (gateEncode(s->g, 0, &buf, &len) != GATE_OK) die("gateEncode failed");
    copy = gateDecode(buf, len, err, sizeof(err));
    gate_free(buf);
    if (copy == NULL) {
        soakFail(s, "resync", err);
        return;
    }
    gateFree(s->replica);
    s->replica = copy;
    s->resyncs++;
}
#endif

/* After every operation. */
static void soakCheck(soak *s, const char *what) {
    const gate *g = s->g;
    int walk = soakWalk(s);
    const char *d;
    char err[160] = "";
    int skip = SKIP_CLOCK | SKIP_REQUESTS | ((s->op & 255) ? SKIP_WEIGHTS : 0);
    int i;

    d = modelDiff(&s->m, g, walk);
    if (d) soakFail(s, what, d);
    if (g->answer_mark > g->next_seq)
        soakFail(s, what, "an answer was used after a request that has not been asked");
    if (g->blind && !s->was_blind) s->blinds++;
    if (!g->blind && s->was_blind) s->seen_again++;
    s->was_blind = g->blind;
    if (gateUnanswered(g) > GATE_MAX_PENDING)
        soakFail(s, what, "more than 4096 requests wait for their answer");
    if (g->nanswered > GATE_MAX_WAITING)
        soakFail(s, what, "more than 16384 answers wait for their turn");
    if (g->missing_run >= GATE_MISSING_RUN)
        soakFail(s, what, "32 answers missing in a row and the gate still sees");
    if (g->blind && (g->candidate || g->serving))
        soakFail(s, what, "a blind gate with proof");
    if (g->candidate && g->serving && g->candidate->gen <= g->serving->gen)
        soakFail(s, what, "candidate older than the serving generation");
    if (gateFreeEffort(g) != 1 + g->npending + (g->candidate != NULL) + (g->serving != NULL))
        soakFail(s, what, "free effort");
    if (g->runtime_tag != EMBEDDING_TAG || g->runtime_clock != EMBEDDING_CLOCK)
        soakFail(s, what, "the embedding layer's fields were touched");
    if (walk) {
        if (!gateCheckInvariants(g, err, sizeof(err))) soakFail(s, what, err);
        if (!gateCheckInvariants(s->replica, err, sizeof(err))) soakFail(s, "replica", err);
        if (gateMemoryUsage(g) != expectBytes(g)) soakFail(s, what, "memory usage");
        if (fingerprint(g, skip) != fingerprint(s->replica, skip))
            soakFail(s, what, "replica differs from primary");
#ifdef TEST_CORE_CODEC
        if ((s->op & 7) == 0) roundTrip(g);
        if (s->op % 499 == 498) soakResync(s);
#endif
    }
    /* A snapshot is frozen: under its number the weights never change. */
    for (i = 0; s->classifier && (s->op & 63) == 0 && i < 2; i++) {
        const gateGeneration *gen = i ? g->serving : g->candidate;
        uint64_t h;

        if (gen == NULL || gen->gen >= SOAK_GENS) continue;
        h = mixModel(1, &gen->model, 0);
        if (s->frozen[gen->gen] == 0) s->frozen[gen->gen] = h;
        if (s->frozen[gen->gen] != h) soakFail(s, what, "a frozen model changed");
    }
    gate_test_checks++;
}

/* The gate before an operation, to see afterwards what the operation did. */
typedef struct soakBefore {
    gateCounters st;
    uint64_t serving; /* generation number, 0: none */
    int32_t policy[MODEL_ROWS];
    int blind;
} soakBefore;

static void soakNote(const soak *s, soakBefore *b) {
    const gate *g = s->g;
    uint32_t r;

    b->st = g->stats;
    b->serving = g->serving ? g->serving->gen : 0;
    b->blind = g->blind;
    for (r = 0; r < g->nrows; r++) b->policy[r] = g->serving ? g->serving->policy[r] : -1;
}

static void soakTally(soak *s, const soakBefore *b) {
    const gate *g = s->g;
    uint32_t r;

    if (g->stats.promotions > b->st.promotions) s->promotions++;
    if (g->stats.promotions > b->st.promotions && b->serving) s->takeovers++;
    if (g->stats.revocations > b->st.revocations) s->revocations++;
    if (g->stats.retired > b->st.retired) s->retired++;
    if (g->stats.missing > b->st.missing) s->missing++;
    if (g->stats.dropped > b->st.dropped) s->dropped++;
    /* While a generation serves, its thresholds only move down. */
    for (r = 0; g->serving && g->serving->gen == b->serving && r < g->nrows; r++) {
        if (b->policy[r] < 0) continue;
        if (g->serving->policy[r] < 0 || g->serving->policy[r] > b->policy[r])
            soakFail(s, "policy", "a threshold moved up");
        if (g->serving->policy[r] < b->policy[r]) s->moved_down++;
    }
}

/* Maintenance of the primary as the module runs it before it looks at a
 * request, checking that gateMaintenanceDue() predicted whether anything
 * would change. Returns what it said. */
static int soakMaintain(soak *s, int64_t wall) {
    int64_t eff = soakEffective(s, wall);
    int due = gateMaintenanceDue(s->g, wall);
    int verify = soakWalk(s);
    uint64_t before = verify ? fingerprint(s->g, SKIP_CLOCK | SKIP_WEIGHTS) : 0;
    soakBefore was;

    soakNote(s, &was);
    gateMaintain(s->g, wall);
    if (verify && due != (fingerprint(s->g, SKIP_CLOCK | SKIP_WEIGHTS) != before))
        soakFail(s, "maintain",
                 due ? "due, but nothing changed" : "not due, but something changed");
    if (s->g->now_ms != eff) soakFail(s, "maintain", "clock");
    /* What the module evaluates a request against is what gatePredict()
     * will find: maintenance leaves nothing to do at the same time. */
    if (gateMaintenanceDue(s->g, wall)) soakFail(s, "maintain", "still due after maintenance");
    modelMaintain(&s->m, wall);
    soakTally(s, &was); /* a timeout or proof that moves on can unblock a promotion */
    return due;
}

/* TICK: all a replica hears of a request that was not recorded, and only
 * if maintenance was due. */
static void soakTick(soak *s, int64_t eff, int due) {
    if (!due) return;
    gateMaintain(s->replica, eff);
    s->maintained++;
}

/* A clue vector and the label the teacher gives it. Buckets 3, 700 and
 * 8191 speak for label 0, 1 and 2; the rest is noise. */
static void soakClues(soak *s, gateSparse *x, int32_t *truth) {
    static const uint32_t vocab[8] = {3, 40, 700, 701, 2000, 5000, 8000, 8191};
    static const float values[4] = {4.0f, 1.0f, 2.0f, -1.0f};
    uint32_t kind = soakRand(s, 10), mask, i, n = 0;
    float strength = 4.0f;

    if (kind < 3) mask = 1u << 0;
    else if (kind < 6) mask = 1u << 2;
    else if (kind < 7) mask = 1u << 7;
    else if (kind < 8) { mask = (1u << 0) | (1u << 1); strength = 1.0f; }
    else mask = 1 + soakRand(s, 255);

    x->idx = gate_malloc(sizeof(uint32_t) * 8);
    x->val = gate_malloc(sizeof(float) * 8);
    if (x->idx == NULL || x->val == NULL) die("out of memory");
    for (i = 0; i < 8; i++) {
        if (!((mask >> i) & 1)) continue;
        x->idx[n] = vocab[i];
        x->val[n] = kind < 8 ? strength : values[soakRand(s, 4)];
        n++;
    }
    x->n = n;
    if (mask & 1) *truth = 0;
    else if (mask & 4) *truth = 1;
    else if (mask & 128) *truth = 2;
    else *truth = (int32_t)soakRand(s, 3);
    if (s->broken || soakRand(s, 20) == 0) *truth = (int32_t)soakRand(s, 3);
}

static double soakScore(soak *s, int32_t *truth) {
    uint32_t kind = soakRand(s, 10);
    double score, right;

    if (kind < 5) score = gate_test_uniform(&s->rng);
    else if (kind < 7) score = s->g->grid[soakRand(s, T)]; /* exactly a threshold */
    else if (kind < 8) score = nextafter(s->g->grid[1 + soakRand(s, T - 1)], 0.0);
    else if (kind < 9) score = 1.0;
    else score = 0.0;
    right = s->broken ? 0.5 : (score >= 0.6 ? 0.98 : 0.5);
    *truth = gate_test_uniform(&s->rng) < right;
    return score;
}

/* Guesses no model made, to get what models rarely give: a candidate that
 * is the more confident of the two (and right when it is), so that it comes
 * to accept more than the serving generation and takes over. */
static gateEval soakMadeUp(soak *s, int32_t *truth) {
    static const double scores[7] = {0.4, 0.6, 0.8, 0.9, 0.97, 0.995, 1.0};
    int32_t cand = (int32_t)soakRand(s, 3);
    int32_t serv = soakRand(s, 10) ? cand : (int32_t)soakRand(s, 3);
    double cand_score = scores[soakRand(s, 7)], serv_score = scores[soakRand(s, 4)];
    double right = s->broken ? 0.3 : (cand_score >= 0.9 ? 0.97 : 0.6);

    *truth = gate_test_uniform(&s->rng) < right ? cand : (int32_t)soakRand(s, 3);
    return guesses(s->g, serv, serv_score, cand, cand_score);
}

/* The teacher's answer to the request recorded last is on its way and will
 * arrive at 'due'. */
static void soakSend(soak *s, int64_t due) {
    if (s->nids == SOAK_IDS) return;
    s->ids[s->nids] = s->asked;
    s->ids[s->nids++].due = due;
}

/* ... or it is not: when it arrives is the teacher's business. A good
 * teacher answers at once, give or take a few milliseconds, and is late
 * now and then; a slow one is late more often than not. */
static void soakTeacher(soak *s) {
    uint32_t kind = soakRand(s, 1000), late;

    if (s->outage) return;                        /* the feedback path is down */
    if (s->partial && soakRand(s, 20)) return;    /* ... or nearly */
    if (s->selective && s->asked_wrong) {         /* a "wrong" that is never sent */
        s->withheld++;
        return;
    }
    if (s->lossy && soakRand(s, 10) == 0) return; /* never gets its answer */
    if (s->burst && soakRand(s, 4)) return;       /* nor do most of a burst */
    if (s->batch) {                               /* held back for the batch */
        s->batched++;
        soakSend(s, s->batch_due);
        return;
    }
    if (s->slow) kind = 600 + kind * 4 / 10;
    if (kind < 930) late = soakRand(s, 6);                  /* at once */
    else if (kind < 970) late = 6 + soakRand(s, GRACE - 6); /* inside GRACE */
    else if (kind < 990) late = GRACE + soakRand(s, 350);   /* proof moves on first */
    else if (kind < 996) late = 600 + soakRand(s, 400);     /* just in time */
    else late = TIMEOUT + soakRand(s, 500);                 /* too late */
    soakSend(s, s->now + late);
}

/* GPREDICT. Returns 1 if the request was recorded: s->asked is its id with
 * the answer the teacher has for it. */
static int soakPredict(soak *s, double u) {
    gate *g = s->g;
    int64_t wall = soakWall(s), eff;
    soakBefore before;
    gatePredictResult res, expect, rres;
    gateSparse x = {0, NULL, NULL}, mine = {0, NULL, NULL};
    gateEval ev, full;
    double score = 0.0;
    int32_t truth;
    int needs, was_full, due, answers;

    due = soakMaintain(s, wall);
    eff = g->now_ms;
    soakNote(s, &before);

    if (!s->classifier) {
        score = soakScore(s, &truth);
    } else if (s->huge) {
        if (gateSparseCopy(&x, &s->big) != GATE_OK) die("out of memory");
        truth = (int32_t)soakRand(s, 3);
    } else {
        soakClues(s, &x, &truth);
    }
    /* Both evaluations: the full one is what the second implementation
     * works with, the one made for this draw is what the module hands to
     * the gate (and to the replica, if the request is recorded). */
    memset(&ev, 0x5a, sizeof(ev));
    if (gateEvaluate(g, s->classifier ? &x : NULL, score, &full) != GATE_OK ||
        gateEvaluateFor(g, s->classifier ? &x : NULL, score, u, &ev) != GATE_OK)
        soakFail(s, "predict", "gateEvaluate failed");
    if (s->made_up) {
        full = soakMadeUp(s, &truth);
        if (gateSparseCopy(&mine, &x) != GATE_OK) die("out of memory");
    } else if (s->classifier) {
        /* The guesses are those of the frozen models. */
        double sc;

        if (full.has_serving != (g->serving != NULL) ||
            full.has_candidate != (g->candidate != NULL))
            soakFail(s, "predict", "guesses for generations that are not there");
        if (g->serving &&
            (gateModelPredict(&g->serving->model, &x, NULL, &sc) != full.serving.label ||
             sc != full.serving.score))
            soakFail(s, "predict", "serving guess");
        if (g->candidate &&
            (gateModelPredict(&g->candidate->model, &x, NULL, &sc) != full.candidate.label ||
             sc != full.candidate.score))
            soakFail(s, "predict", "candidate guess");
        if (gateSparseCopy(&mine, &x) != GATE_OK) die("out of memory");
    }
    /* The candidate's guess is left out exactly when the serving generation
     * is going to answer by itself, which the second implementation decides
     * on its own. Made-up guesses are cut down the same way. */
    answers = modelWouldAnswer(&s->m, u, &full);
    if (s->made_up) {
        ev = full;
        if (answers) {
            ev.has_candidate = 0;
            ev.candidate.label = -1;
            ev.candidate.score = 0.0;
        }
    } else if (ev.has_serving != full.has_serving || ev.serving.label != full.serving.label ||
               ev.serving.score != full.serving.score)
    {
        soakFail(s, "predict", "gateEvaluateFor: serving guess");
    } else if (answers ? (ev.has_candidate != 0 || ev.candidate.label != -1 ||
                          ev.candidate.score != 0.0)
                       : (ev.has_candidate != full.has_candidate ||
                          ev.candidate.label != full.candidate.label ||
                          ev.candidate.score != full.candidate.score))
    {
        soakFail(s, "predict", answers ? "gateEvaluateFor: a guess nobody needs"
                                       : "gateEvaluateFor: candidate guess");
    }
    needs = gatePredictNeedsFeatures(g, u, &ev);
    was_full = gateFull(g);
    if (gatePredict(g, wall, u, &ev, s->classifier ? &mine : NULL, &res) != GATE_OK)
        soakFail(s, "predict", "gatePredict failed");
    modelPredict(&s->m, wall, u, &full, x.n, truth, &expect);
    if (!was_full && answers != (res.action == GATE_ACTION_ACCEPT))
        soakFail(s, "predict", "the request the serving generation answers is the ACCEPT");
    s->left_out += (uint64_t)(res.action == GATE_ACTION_ACCEPT && full.has_candidate);

    if (res.action != expect.action || res.label != expect.label || res.seq != expect.seq)
        soakFail(s, "predict", "reply differs from the model");
    if (needs != (s->classifier && res.seq != 0))
        soakFail(s, "predict", "gatePredictNeedsFeatures disagrees with gatePredict");
    if (res.action == GATE_ACTION_ACCEPT &&
        (res.seq != 0 || g->paused || g->serving == NULL || was_full || u < s->cfg.audit))
        soakFail(s, "predict", "an ACCEPT that should not be");
    if (res.action == GATE_ACTION_AUDIT && (res.seq == 0 || !(u < s->cfg.audit) || g->paused))
        soakFail(s, "predict", "an AUDIT that should not be");
    if (was_full && (res.action != GATE_ACTION_ABSTAIN || res.seq != 0))
        soakFail(s, "predict", "a full gate looked at the request");
    if (before.blind && res.action != GATE_ACTION_ABSTAIN)
        soakFail(s, "predict", "a blind gate answered");
    if (before.blind && res.seq == 0 && !was_full && gateUnanswered(g) < GATE_TRAIN_PENDING)
        soakFail(s, "predict", "a blind gate gave no id");
    if (s->classifier) {
        /* Recorded: the gate took the clues. Otherwise they are untouched. */
        if (res.seq ? (mine.n != 0 || mine.idx != NULL || mine.val != NULL)
                    : (mine.n != x.n || (x.n && mine.idx == NULL)))
            soakFail(s, "predict", "ownership of the clues");
        gateSparseFree(&mine);
    }

    if (res.seq != 0) {
        /* PREDICT: the realized clock, draw, guesses and request number,
         * and the clues because the request was recorded. The replica is
         * told nothing else, not even that maintenance was due: it does its
         * own. It takes the primary's request number over; here it must
         * have arrived at the same one by itself. */
        uint64_t before_seq = s->replica->next_seq;

        if (s->classifier && gateSparseCopy(&mine, &x) != GATE_OK) die("out of memory");
        if (before_seq != res.seq)
            soakFail(s, "predict", "replica counts its requests differently");
        /* What the module checks before it takes the number over: one
         * that was not used yet and that a gate may hand out. */
        if (res.seq < before_seq || res.seq >= GATE_COUNT_MAX)
            soakFail(s, "predict", "a request number a replica refuses");
        s->replica->next_seq = res.seq;
        if (gatePredict(s->replica, eff, u, &ev, s->classifier ? &mine : NULL, &rres) != GATE_OK ||
            rres.seq != res.seq || rres.action != res.action)
        {
            soakFail(s, "predict", "replica replied differently");
            if (rres.seq == 0) s->replica->next_seq = before_seq; /* nothing took the number */
        }
        gateSparseFree(&mine);
        s->asked.epoch = g->epoch;
        s->asked.seq = res.seq;
        s->asked.due = s->now;
        s->asked.truth = truth;
        /* Would the teacher's answer say "wrong" to the generation in
         * charge (the one that serves, else the one under test)? */
        if (!s->classifier) s->asked_wrong = !truth;
        else if (full.has_serving) s->asked_wrong = full.serving.label != truth;
        else s->asked_wrong = full.has_candidate && full.candidate.label != truth;
        s->blind_asks += (uint64_t)before.blind;
    } else {
        soakTick(s, eff, due);
    }
    gateSparseFree(&x);

    if (res.action == GATE_ACTION_ACCEPT) s->accepts++;
    if (res.action == GATE_ACTION_AUDIT) s->audits++;
    if (was_full) {
        s->full++;
        if (gateUnanswered(g) < GATE_MAX_PENDING) s->full_bytes++;
    } else if (s->classifier && res.seq == 0 && res.action == GATE_ACTION_ABSTAIN) {
        s->untrained++;
    }
    soakTally(s, &before);
    soakCheck(s, "predict");
    return res.seq != 0;
}

/* GLEARN with whatever id, on all three. */
static void soakLearn(soak *s, const soakId *id, const char *what) {
    int64_t wall = soakWall(s), eff = soakEffective(s, wall);
    int32_t truth = id->truth;
    uint64_t mark = s->g->answer_mark, alarms;
    soakBefore before;
    uint32_t i;
    int used;

    /* The teacher has one answer for a request. Whoever delivers it, and
     * whenever, it is the one the model was told when the request was
     * asked. */
    for (i = 0; id->epoch == s->m.epoch && i < s->m.n; i++)
        if (s->m.req[i].seq == id->seq) truth = s->m.req[i].truth;
    soakNote(s, &before);
    alarms = s->m.unchecked_by_answer;
    used = gateLearn(s->g, wall, id->epoch, id->seq, truth);
    if (used != modelLearn(&s->m, wall, id->epoch, id->seq, truth))
        soakFail(s, what, "gateLearn differs from the model");
    if (gateLearn(s->replica, eff, id->epoch, id->seq, truth) != used)
        soakFail(s, what, "replica used the answer differently");
    if (used && s->g->missing_run)
        soakFail(s, what, "a run of missing answers after an answer that was used");
    if (used && s->g->answer_mark != s->g->next_seq)
        soakFail(s, what, "an answer was used and the mark is not at the next request");
    /* An answer that is used makes a blind gate see. The one exception:
     * the answer was the disagreement by which the drift detector found
     * that the gate cannot be checked. */
    if (used && s->g->blind && s->m.unchecked_by_answer == alarms)
        soakFail(s, what, "blind after an answer that was used");
    if (!used && (s->g->blind != before.blind && !s->g->blind))
        soakFail(s, what, "an answer that was not used made the gate see");
    if (!used && s->g->answer_mark != mark)
        soakFail(s, what, "an answer that was not used moved the mark");
    soakTally(s, &before);
    soakCheck(s, what);
}

/* An answer that is due arrives: the one that is due first or, with 'any',
 * any one of them (a client that collects answers and passes them on in
 * whatever order). Returns 0 if none is due. */
static int soakAnswer(soak *s, int any) {
    uint32_t i, best = s->nids, ndue = 0, pick;
    int used;

    for (i = 0; i < s->nids; i++) {
        if (s->ids[i].due > s->now) continue;
        ndue++;
        if (best == s->nids || s->ids[i].due < s->ids[best].due) best = i;
    }
    if (best == s->nids) return 0;
    if (any) {
        pick = soakRand(s, ndue);
        for (i = 0; i < s->nids; i++) {
            if (s->ids[i].due > s->now) continue;
            if (pick-- == 0) break;
        }
        best = i;
    }
    s->last = s->ids[best];
    s->ids[best] = s->ids[--s->nids];
    used = (int)s->g->stats.learned;
    soakLearn(s, &s->last, "answer");
    if ((int)s->g->stats.learned == used) s->too_late++;
    return 1;
}

/* Answers nobody asked for. */
static void soakStrayAnswer(soak *s) {
    soakId id;

    id.epoch = s->g->epoch;
    id.due = s->now;
    id.truth = (int32_t)soakRand(s, s->classifier ? 3 : 2);
    switch (soakRand(s, 5)) {
    case 0: id.seq = 0; break;
    case 1: id.seq = s->g->next_seq + soakRand(s, 3); break;
    case 2: /* a waiting request under another gate's epoch */
        id.seq = s->g->oldest ? s->g->oldest->seq : 1;
        id.epoch ^= 1;
        break;
    case 3: id = s->last; break; /* an answer given twice */
    default: id.seq = 1 + soakRand(s, (uint32_t)s->g->next_seq); break; /* one that overtakes */
    }
    soakLearn(s, &id, "stray answer");
}

static void soakTrain(soak *s) {
    int64_t wall = soakWall(s), eff = soakEffective(s, wall);
    gateSparse x;
    int32_t truth;

    soakClues(s, &x, &truth);
    if (gateTrain(s->g, wall, &x, truth) != GATE_OK) soakFail(s, "train", "gateTrain failed");
    if (gateTrain(s->replica, eff, &x, truth) != GATE_OK) soakFail(s, "train", "replica");
    modelTrain(&s->m, wall);
    gateSparseFree(&x);
    soakCheck(s, "train");
}

static void soakAdmin(soak *s, int what) {
    int64_t wall = soakWall(s), eff = soakEffective(s, wall);
    uint64_t epoch = gate_test_rand64(&s->rng);

    switch (what) {
    case 0:
    case 1:
        if (what) gatePause(s->g, wall); else gateResume(s->g, wall);
        if (what) gatePause(s->replica, eff); else gateResume(s->replica, eff);
        modelPause(&s->m, wall, what);
        break;
    case 2:
        /* A reset and a recovery set the clock: here, now and then, back.
         * The replica is told the same one. */
        gateReset(s->g, wall, epoch);
        gateReset(s->replica, wall, epoch);
        modelForget(&s->m, wall, epoch, 1);
        s->resets++;
        break;
    default:
        gateRecover(s->g, wall, epoch);
        gateRecover(s->replica, wall, epoch);
        modelForget(&s->m, wall, epoch, 0);
        s->recoveries++;
        break;
    }
    if (what >= 2 && (s->g->now_ms != wall || s->replica->now_ms != wall))
        soakFail(s, "administration", "clock after a reset or a recovery");
    if (what >= 2 && (s->g->blind || s->g->missing_run))
        soakFail(s, "administration", "blind after a reset or a recovery");
    soakCheck(s, "administration");
}

/* An open request and, within the same millisecond, more answers behind it
 * than may wait: proof has to move on although the request is young. */
static void soakWaitingLimit(soak *s) {
    uint32_t i;

    if (soakPredict(s, U_SPOT)) soakSend(s, s->now + GRACE + 1 + soakRand(s, 600));
    for (i = 0; i < GATE_MAX_WAITING + 40 && !s->failed; i++) {
        if (soakPredict(s, U_SPOT)) {
            soakId id = s->asked;

            soakLearn(s, &id, "answer at once");
        }
    }
}

/* The feedback path goes down for a while: no answer gets through, and
 * what is on its way is lost too. */
static void soakOutage(soak *s) {
    s->outage = 2000 + soakRand(s, 2000);
    s->nids = 0;
    s->batch = 0;
    s->batch_due = 0;
    s->flush = 0;
    s->outages++;
}

/* For a while the teacher holds its answers back and then sends them all
 * at one moment, in any order. */
static void soakBatch(soak *s, uint32_t ops) {
    if (s->batch || s->batch_due || s->flush) return;
    s->batch = ops;
    s->batch_due = s->now + 2 * (int64_t)ops + soakRand(s, 100);
}

/* Everything that waits runs out of time at once. */
static void soakJump(soak *s) {
    int due;

    s->now += TIMEOUT + soakRand(s, 50);
    due = soakMaintain(s, s->now);
    soakTick(s, s->g->now_ms, due);
    soakCheck(s, "timeout");
}

static void soakStep(soak *s) {
    uint32_t dice = soakRand(s, 100000);
    double u = gate_test_uniform(&s->rng);

    switch (soakRand(s, 8)) {
    case 0: u = 0.0; break;
    case 1: u = s->cfg.audit; break; /* the largest draw that is no spot check */
    default: break;
    }
    /* A candidate lives on spot checks: give the one that is to take over
     * more of them. */
    if (s->made_up && soakRand(s, 2)) u *= s->cfg.audit;
    if (s->broken) s->broken--;
    if (s->lossy) s->lossy--;
    if (s->slow) s->slow--;
    if (s->outage) s->outage--;
    if (s->partial) s->partial--;
    if (s->selective) s->selective--;
    if (s->batch) s->batch--;
    if (s->burst) {
        /* No answer arrives and no time passes: the gate fills up. */
        s->burst--;
        if (soakPredict(s, U_SPOT)) soakTeacher(s);
        return;
    }
    if (s->flush) {
        /* A batch arrives: one answer after the other at the same moment,
         * in any order, and nothing in between. */
        s->flush--;
        if (!soakAnswer(s, 1)) s->flush = 0;
        return;
    }
    s->huge = 0;
    s->now += soakRand(s, 3);
    if (s->batch_due && s->now >= s->batch_due) {
        uint32_t i;

        for (i = 0; i < s->nids; i++) s->flush += s->ids[i].due <= s->now;
        s->batch = 0;
        s->batch_due = 0;
        s->batches++;
    }
    if (dice < 40000) {
        if (soakPredict(s, u)) soakTeacher(s);
    } else if (dice < 85000) {
        /* Mostly the answer that is due first; now and then another. */
        if (!soakAnswer(s, soakRand(s, 4) == 0) && soakPredict(s, u)) soakTeacher(s);
    } else if (dice < 88000) {
        soakStrayAnswer(s);
    } else if (dice < 93000 && s->classifier) {
        soakTrain(s);
    } else if (dice < 98000) {
        /* A request that is not recorded: all that is left of it is the
         * maintenance it began with. */
        int64_t wall = soakWall(s);
        int due = soakMaintain(s, wall);

        soakTick(s, s->g->now_ms, due);
        soakCheck(s, "maintain");
    } else if (dice < 98100) {
        soakAdmin(s, 1);
    } else if (dice < 98500) {
        soakAdmin(s, 0);
    } else if (dice < 98530) {
        soakAdmin(s, 2);
    } else if (dice < 98560) {
        soakAdmin(s, 3);
    } else if (dice < 98610) {
        soakJump(s);
    } else if (dice < 98630) {
        s->broken = 800;
    } else if (dice < 98660) {
        s->lossy = 500;
    } else if (dice < 98666) {
        s->burst = GATE_MAX_PENDING + 200;
    } else if (dice < 98672 && s->classifier) {
        s->burst = 300;
        s->huge = 1;
    } else if (dice < 98684) {
        s->slow = 1500;
    } else if (dice < 98687) {
        soakOutage(s);
    } else if (dice < 98702) {
        soakBatch(s, 100 + soakRand(s, 200));
    } else if (dice < 98708) {
        s->selective = 1500;
    } else if (dice < 98712) {
        s->partial = 1500;
    } else {
        if (soakPredict(s, u)) soakTeacher(s);
    }
}

/* 'kind': 0 a score gate, 1 a classifier, 2 a classifier with made-up
 * guesses. */
static void soakRun(soak *s, int kind, uint64_t seed, uint32_t ops) {
    int classifier = kind != 0;
    char err[128];
    uint32_t i, hidden = 0;
    int waited = 0, outage = 0, batched = 0;

    memset(s, 0, sizeof(*s));
    s->classifier = classifier;
    s->made_up = kind == 2;
    s->rng = seed;
    s->now = T0;
    if (classifier) {
        classConfig(&s->cfg, 3, 30);
        s->cfg.class_alpha[1] = 0.1;
        s->cfg.class_defer[2] = 1;
    } else {
        baseConfig(&s->cfg);
    }
    if (gateConfigValidate(&s->cfg, err, sizeof(err)) != GATE_OK) die(err);
    s->g = gateCreate(&s->cfg, LINEAGE, EPOCH, T0);
    s->replica = gateCreate(&s->cfg, LINEAGE, EPOCH, T0);
    s->ids = malloc(sizeof(soakId) * SOAK_IDS);
    s->big.n = GATE_DIM;
    s->big.idx = malloc(sizeof(uint32_t) * GATE_DIM);
    s->big.val = malloc(sizeof(float) * GATE_DIM);
    if (!s->g || !s->replica || !s->ids || !s->big.idx || !s->big.val) die("out of memory");
    s->g->runtime_tag = EMBEDDING_TAG; /* the module's: see soakCheck() */
    s->g->runtime_clock = EMBEDDING_CLOCK;
    for (i = 0; i < GATE_DIM; i++) {
        s->big.idx[i] = i;
        s->big.val[i] = 0.001f;
    }
    modelInit(&s->m, &s->cfg, s->g);
    s->last.epoch = EPOCH;
    s->last.seq = 1;

    for (s->op = 0; s->op < ops && !s->failed; s->op++) {
        soakStep(s);
        if (s->huge && s->burst == 0) s->huge = 0;
        /* Once in every walk, wherever it is by then: an outage; a teacher
         * that goes wrong and hides it (no answer that says "wrong" for a
         * while, then the time of everything that waits is up); more
         * answers than may wait; a batch with disagreements in it. */
        if (!outage && s->op >= ops / 4) {
            soakOutage(s);
            outage = 1;
        }
        if (!hidden && s->op >= ops / 8 * 3 && s->burst == 0 && s->outage == 0 && s->g->serving) {
            s->selective = s->broken = 400;
            hidden = s->op + 400;
        }
        if (hidden > 1 && s->op == hidden) {
            soakJump(s);
            hidden = 1;
        }
        if (!waited && s->op >= ops / 2 && s->burst == 0 && (s->g->candidate || s->g->serving)) {
            soakWaitingLimit(s);
            waited = 1;
        }
        if (!batched && s->op >= ops / 8 * 5 && s->burst == 0 && s->outage == 0 && s->g->serving) {
            soakBatch(s, 150);
            s->broken = 60;
            batched = 1;
        }
    }
    CHECK(!s->failed);
    /* The replica holds what the primary holds, at the end too. */
    CHECK(fingerprint(s->g, SKIP_CLOCK | SKIP_REQUESTS) ==
          fingerprint(s->replica, SKIP_CLOCK | SKIP_REQUESTS));
    s->passes = s->m.passes;
    s->limit_passes = s->m.limit_passes;
    s->taken_back = s->m.taken_back;
    s->late_answers = s->m.late_answers;
    s->late_missing = s->m.late_missing;
    s->lost_answers = s->m.lost_answers;
    s->worse = s->m.worse;
    s->unchecked = s->m.unchecked;
    s->unchecked_by_answer = s->m.unchecked_by_answer;
    s->late_reports = s->m.late_reports;
    s->numbered = s->m.numbered;
    s->missed = s->m.missed;
    s->missed_out = s->m.missed_out;
    s->held_back = s->m.held_back;

    gateFree(s->g);
    gateFree(s->replica);
    gateConfigFree(&s->cfg);
    modelFree(&s->m);
    free(s->ids);
    free(s->big.idx);
    free(s->big.val);
}

/* Where the walks of several seeds went, added up. */
static void soakAdd(soak *sum, const soak *s) {
    sum->accepts += s->accepts;
    sum->audits += s->audits;
    sum->promotions += s->promotions;
    sum->takeovers += s->takeovers;
    sum->revocations += s->revocations;
    sum->retired += s->retired;
    sum->missing += s->missing;
    sum->dropped += s->dropped;
    sum->full += s->full;
    sum->full_bytes += s->full_bytes;
    sum->untrained += s->untrained;
    sum->moved_down += s->moved_down;
    sum->resets += s->resets;
    sum->recoveries += s->recoveries;
    sum->maintained += s->maintained;
    sum->passes += s->passes;
    sum->limit_passes += s->limit_passes;
    sum->taken_back += s->taken_back;
    sum->late_answers += s->late_answers;
    sum->late_missing += s->late_missing;
    sum->too_late += s->too_late;
    sum->blinds += s->blinds;
    sum->blind_asks += s->blind_asks;
    sum->seen_again += s->seen_again;
    sum->outages += s->outages;
    sum->left_out += s->left_out;
    sum->lost_answers += s->lost_answers;
    sum->worse += s->worse;
    sum->unchecked += s->unchecked;
    sum->unchecked_by_answer += s->unchecked_by_answer;
    sum->late_reports += s->late_reports;
    sum->numbered += s->numbered;
    sum->missed += s->missed;
    sum->missed_out += s->missed_out;
    sum->held_back += s->held_back;
    sum->batches += s->batches;
    sum->batched += s->batched;
    sum->withheld += s->withheld;
    sum->resyncs += s->resyncs;
}

static const char *const soak_names[3] = {"soak_score", "soak_classifier", "soak_takeovers"};
static const uint32_t soak_ops[3] = {60000, 30000, 30000};

/* The walks of one kind for some seeds; where they went, added up. */
static void soakSeeds(int kind, const uint64_t *seeds, size_t n, soak *sum, int print) {
    static soak s;
    size_t i;

    memset(sum, 0, sizeof(*sum));
    for (i = 0; i < n; i++) {
        soakRun(&s, kind, seeds[i], soak_ops[kind]);
        soakAdd(sum, &s);
        if (!print) continue;
        printf("  %s seed %llu: %s. accept %llu (%llu without the candidate's guess) audit %llu "
               "promoted %llu (took over %llu, held back %llu) revoked "
               "%llu retired %llu (%llu with missed requests, %llu missed); moved on %llu (limit "
               "%llu), taken back %llu, late answers "
               "%llu, too late %llu, missing after moving on %llu; blind %llu (asked %llu, saw "
               "again %llu), outages %llu; spot checks %llu, alarms: worse %llu, unchecked %llu "
               "(%llu by an answer), reports after their block %llu, lost answers outside a run "
               "%llu; batches %llu (%llu answers), withheld %llu; full %llu (bytes %llu), not "
               "kept %llu, ticks %llu, "
               "thresholds moved down %llu, resets %llu, recoveries %llu, missing %llu, dropped "
               "%llu, resyncs %llu\n",
               soak_names[kind], (unsigned long long)seeds[i], s.failed ? "FAILED" : "ok",
               (unsigned long long)s.accepts, (unsigned long long)s.left_out,
               (unsigned long long)s.audits,
               (unsigned long long)s.promotions, (unsigned long long)s.takeovers,
               (unsigned long long)s.held_back,
               (unsigned long long)s.revocations, (unsigned long long)s.retired,
               (unsigned long long)s.missed_out, (unsigned long long)s.missed,
               (unsigned long long)s.passes, (unsigned long long)s.limit_passes,
               (unsigned long long)s.taken_back, (unsigned long long)s.late_answers,
               (unsigned long long)s.too_late, (unsigned long long)s.late_missing,
               (unsigned long long)s.blinds, (unsigned long long)s.blind_asks,
               (unsigned long long)s.seen_again, (unsigned long long)s.outages,
               (unsigned long long)s.numbered, (unsigned long long)s.worse,
               (unsigned long long)s.unchecked, (unsigned long long)s.unchecked_by_answer,
               (unsigned long long)s.late_reports, (unsigned long long)s.lost_answers,
               (unsigned long long)s.batches, (unsigned long long)s.batched,
               (unsigned long long)s.withheld,
               (unsigned long long)s.full, (unsigned long long)s.full_bytes,
               (unsigned long long)s.untrained, (unsigned long long)s.maintained,
               (unsigned long long)s.moved_down, (unsigned long long)s.resets,
               (unsigned long long)s.recoveries, (unsigned long long)s.missing,
               (unsigned long long)s.dropped, (unsigned long long)s.resyncs);
    }
}

TEST(soak_score) {
    static const uint64_t seeds[] = {6, 10};
    static soak sum;

    soakSeeds(0, seeds, COUNTOF(seeds), &sum, 0);
    /* The walks must have been through everything they are there for. */
    CHECK(sum.accepts >= 1000 && sum.audits >= 1000);
    CHECK(sum.promotions >= 10);
    CHECK(sum.moved_down >= 10); /* thresholds proven after promotion */
    CHECK(sum.revocations >= 3);
    CHECK(sum.missing >= 50 && sum.dropped >= 1);
    CHECK(sum.full >= 100);
    CHECK(sum.resets >= 5 && sum.recoveries >= 5);
    CHECK(sum.maintained >= 50); /* maintenance a replica had to be told of */
    /* Late answers: proof moved on, guesses were taken back, answers came
     * too late or never; the waiting limit; outages. */
    CHECK(sum.passes >= 500 && sum.taken_back >= 200 && sum.late_answers >= 300);
    CHECK(sum.too_late >= 50 && sum.late_missing >= 50);
    CHECK(sum.limit_passes >= 1);
    CHECK(sum.outages >= 2 && sum.blinds >= 2 && sum.blind_asks >= 100 && sum.seen_again >= 2);
    /* The drift detector: spot checks numbered, both alarms (the second
     * one also raised by an answer), disagreements and lost answers
     * reported after their block of 64 had ended. Missing answers that
     * were no part of a run. Answers in batches; "wrong" withheld. */
    CHECK(sum.numbered >= 1000 && sum.worse >= 2 && sum.unchecked >= 2);
    CHECK(sum.unchecked_by_answer >= 1 && sum.late_reports >= 50);
    CHECK(sum.lost_answers >= 1000);
    CHECK(sum.batches >= 5 && sum.batched >= 500 && sum.withheld >= 500);
}

TEST(soak_classifier) {
    static const uint64_t seeds[] = {9, 19, 23};
    static soak sum;

    soakSeeds(1, seeds, COUNTOF(seeds), &sum, 0);
    CHECK(sum.accepts >= 300 && sum.audits >= 100);
    CHECK(sum.promotions >= 5);
    CHECK(sum.moved_down >= 5);
    CHECK(sum.retired >= 1);     /* a snapshot dropped at its budget */
    CHECK(sum.revocations >= 1);
    CHECK(sum.missing >= 50 && sum.dropped >= 20);
    CHECK(sum.full >= 100 && sum.full_bytes >= 10);
    CHECK(sum.untrained >= 10);  /* the limit on requests kept for training */
    CHECK(sum.resets >= 3 && sum.recoveries >= 3);
    CHECK(sum.passes >= 200 && sum.taken_back >= 50 && sum.late_answers >= 100);
    CHECK(sum.too_late >= 20 && sum.late_missing >= 20);
    CHECK(sum.blinds >= 1 && sum.blind_asks >= 20 && sum.seen_again >= 1);
    CHECK(sum.left_out >= 100); /* answered without asking the candidate */
    /* Requests a snapshot could have been tested on, and snapshots that
     * were dropped with some; a takeover, and takeovers that only the
     * rule that was would have made; both drift alarms. */
    CHECK(sum.missed >= 1000 && sum.missed_out >= 3);
    CHECK(sum.takeovers >= 1 && sum.held_back >= 1);
    CHECK(sum.numbered >= 1000 && sum.worse >= 1 && sum.unchecked >= 1);
    CHECK(sum.unchecked_by_answer >= 1 && sum.late_reports >= 50);
    CHECK(sum.lost_answers >= 1000 && sum.batches >= 5 && sum.withheld >= 500);
}

TEST(soak_takeovers) {
    static const uint64_t seeds[] = {14, 18, 23};
    static soak sum;

    soakSeeds(2, seeds, COUNTOF(seeds), &sum, 0);
    CHECK(sum.promotions >= 10);
    CHECK(sum.takeovers >= 3); /* a candidate replacing a serving generation */
    CHECK(sum.held_back >= 1); /* ... and one that did not, by what the serving one answers now */
    CHECK(sum.revocations >= 1);
    CHECK(sum.passes >= 200 && sum.taken_back >= 50);
    CHECK(sum.left_out >= 100);
    CHECK(sum.missed >= 1000 && sum.missed_out >= 1);
    CHECK(sum.unchecked >= 1 && sum.late_reports >= 50);
}

int main(int argc, char **argv) {
    uint32_t r;

    gateSetAllocator(testMalloc, testCalloc, testRealloc, testFree);
    for (r = 0; r < GATE_MAX_CLASSES; r++) blank_policy[r] = -99;

    if (argc > 1) {
        /* Extra seeds for the randomized walks: "17" runs the three walks
         * with seed 17, "score:17", "classifier:17" or "takeovers:17" one. */
        int a, kind;

        for (a = 1; a < argc; a++) {
            const char *arg = argv[a], *colon = strchr(arg, ':');
            uint64_t seed = strtoull(colon ? colon + 1 : arg, NULL, 10);
            static soak sum;

            for (kind = 0; kind < 3; kind++) {
                const char *name = soak_names[kind] + 5; /* without "soak_" */

                if (colon && (strlen(name) != (size_t)(colon - arg) ||
                              strncmp(arg, name, strlen(name)) != 0))
                    continue;
                gate_test_current = soak_names[kind];
                soakSeeds(kind, &seed, 1, &sum, 1);
                gate_test_checks++;
                if (live_blocks != 0) {
                    gate_test_failures++;
                    printf("  %s: %ld blocks were not released\n", soak_names[kind], live_blocks);
                    live_blocks = 0;
                }
            }
        }
        return TEST_REPORT();
    }

    RUN_LEAKFREE(expected_numbers);
    RUN_LEAKFREE(names);
    RUN_LEAKFREE(config_defaults);
    RUN_LEAKFREE(config_classes);
    RUN_LEAKFREE(config_label_with_zero_byte);
    RUN_LEAKFREE(config_validate);
    RUN_LEAKFREE(config_copy_and_free);
    RUN_LEAKFREE(grids);
    RUN_LEAKFREE(bar);
    RUN_LEAKFREE(score_life);
    RUN_LEAKFREE(continued_testing);
    RUN_LEAKFREE(coverage_average);
    RUN_LEAKFREE(evaluate);
    RUN_LEAKFREE(evaluate_for);
    RUN_LEAKFREE(classifier_life);
    RUN_LEAKFREE(frozen_guesses);
    RUN_LEAKFREE(asked_before_snapshot);
    RUN_LEAKFREE(proof_survives_promotion);
    RUN_LEAKFREE(answers_wait_for_their_turn);
    RUN_LEAKFREE(answer_order_does_not_matter);
    RUN_LEAKFREE(answer_order_does_not_matter_classifier);
    RUN_LEAKFREE(feedback_ids);
    RUN_LEAKFREE(grace_moves_on);
    RUN_LEAKFREE(grace_is_a_quarter);
    RUN_LEAKFREE(waiting_limit);
    RUN_LEAKFREE(late_answer_restores_the_proof);
    RUN_LEAKFREE(late_answers_restore_the_counters);
    RUN_LEAKFREE(late_answer_both_generations);
    RUN_LEAKFREE(late_answer_after_promotion);
    RUN_LEAKFREE(late_answer_after_budget);
    RUN_LEAKFREE(late_answer_after_drift_alarm);
    RUN_LEAKFREE(late_answer_after_reset_and_recover);
    RUN_LEAKFREE(late_answer_wrong);
    RUN_LEAKFREE(drift_hears_answers_as_they_arrive);
    RUN_LEAKFREE(late_answer_proves_a_threshold);
    RUN_LEAKFREE(late_answer_triggers_a_promotion);
    RUN_LEAKFREE(timeouts_end_once);
    RUN_LEAKFREE(missing_is_an_error_for_the_proof_only);
    RUN_LEAKFREE(blind_run);
    RUN_LEAKFREE(blind_score_gate);
    RUN_LEAKFREE(blind_classifier);
    RUN_LEAKFREE(blind_records_up_to_the_training_limit);
    RUN_LEAKFREE(blind_outage_uses_no_generation);
    RUN_LEAKFREE(blind_cleared_by_reset_and_recover);
    RUN_LEAKFREE(drift_alarm_clears_the_run);
    RUN_LEAKFREE(blind_then_queue_head);
    RUN_LEAKFREE(policy_is_lowest_proven);
    RUN_LEAKFREE(every_threshold_on_its_own);
    RUN_LEAKFREE(guess_of_a_deferred_label_is_no_proof);
    RUN_LEAKFREE(bar_per_generation);
    RUN_LEAKFREE(replacing_needs_more_accepts);
    RUN_LEAKFREE(candidate_budget);
    RUN_LEAKFREE(score_candidate_has_no_budget);
    RUN_LEAKFREE(drift_score);
    RUN_LEAKFREE(drift_classifier);
    RUN_LEAKFREE(promotion_like_with_like);
    RUN_LEAKFREE(promotion_counts_every_label);
    RUN_LEAKFREE(promotion_counts_the_answer_for_both);
    RUN_LEAKFREE(lost_answers_are_not_an_outage);
    RUN_LEAKFREE(missed_requests);
    RUN_LEAKFREE(limits);
    RUN_LEAKFREE(counters_at_their_limit);
    RUN_LEAKFREE(dead_queue_head_is_dropped);
    RUN_LEAKFREE(drift_spot_numbers);
    RUN_LEAKFREE(drift_order_and_batches);
    RUN_LEAKFREE(drift_block_ended);
    RUN_LEAKFREE(drift_blocks);
    RUN_LEAKFREE(drift_lost_spot_checks);
    RUN_LEAKFREE(drift_paused_gate);
    RUN_LEAKFREE(drift_generation_replaced);
    RUN_LEAKFREE(score_gate_refuses_labels_and_clues);
    RUN_LEAKFREE(score_that_is_not_a_number);
    RUN_LEAKFREE(invariants_of_the_new_fields);
    RUN_LEAKFREE(full_gate);
    RUN_LEAKFREE(full_counts_unanswered);
    RUN_LEAKFREE(training_only_limit);
    RUN_LEAKFREE(training_limit_counts_unanswered);
    RUN_LEAKFREE(full_by_bytes);
    RUN_LEAKFREE(pause);
    RUN_LEAKFREE(reset);
    RUN_LEAKFREE(recover);
    RUN_LEAKFREE(clues_needed_and_owned);
    RUN_LEAKFREE(predict_refuses_bad_input);
    RUN_LEAKFREE(clock);
    RUN_LEAKFREE(reset_and_recover_set_the_clock);
    RUN_LEAKFREE(clock_at_the_ends);
    RUN_LEAKFREE(maintenance_due);
    RUN_LEAKFREE(tiny_alpha);
    RUN_LEAKFREE(memory_usage);
    RUN_LEAKFREE(free_with_waiting_requests);
    RUN_LEAKFREE(constructors);
    RUN_LEAKFREE(embedding_fields_are_left_alone);
    RUN_LEAKFREE(allocation_failures);
    RUN_LEAKFREE(soak_score);
    RUN_LEAKFREE(soak_classifier);
    RUN_LEAKFREE(soak_takeovers);
    return TEST_REPORT();
}
