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
 * happens within a few dozen requests. The library allocates through a
 * counting allocator: every test must give back each block it took.
 *
 * WHERE THE EXPECTED NUMBERS COME FROM. None was produced by gate_core.c.
 *
 * The bar of generation g is log(g (g + 1)) + log(tested rows * entry
 * points) - log(delta). With delta 0.2 the wealth a region must reach is
 *
 *     score gate (32 entry points), generation 1, 2, 3:      320, 960, 1920
 *     two tested labels (4 entry points), generation 1, 2:    80, 240
 *     three tested labels, generation 1, 2, 3:               120, 360, 720
 *
 * A region that has seen n correct answers and k wrong ones holds
 *
 *     wealth(n, k) = 1/4 sum_j (1 + c_j alpha / (1 - alpha))^n (1 - c_j)^k
 *
 * with c = 0.1, 0.25, 0.5, 0.9 (gate_stats.h). The smallest n that reaches
 * a target was computed with exact rational arithmetic:
 *
 *     alpha 0.2, target 320:  k = 0: 35   k = 1: 47   k = 2: 57   k = 3: 68
 *     alpha 0.2, target 960: 41      target 1920: 44
 *     alpha 0.2, target 80:   k = 0: 28   k = 10: 107
 *     alpha 0.2, target 240: 34      target 120: 30
 *     alpha 0.1, target 80: 60
 *
 * The first one by hand: for alpha 0.2 a correct answer multiplies the four
 * bets by 1.025, 1.0625, 1.125 and 1.225. After 35 of them they hold 2.4,
 * 8.4, 61.7 and 1215.6: a quarter of the sum is 322. After 34 it is 264.
 * needCorrect() below evaluates the same formula in floating point and the
 * first test requires that it reproduces this table.
 *
 * The drift detector for alpha 0.2 multiplies its two statistics by 2 and
 * by 3 on a wrong spot check and by 0.75 and 0.5 on a correct one (never
 * letting them start below 1), and fires when their average reaches 1000:
 * after 7 wrong spot checks in a row ((128 + 2187) / 2 = 1157.5; 6 give
 * 396.5).
 *
 * The randomized test at the end compares the gate, operation by operation,
 * with a second implementation of the rules of gate_core.h that is written
 * for obviousness instead of speed (one array, linear scans).
 */

#include <float.h>
#include "testutil.h"
#include "gate_core.h"

#define T GATE_THRESHOLDS
#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

#define ALPHA 0.2
#define DELTA 0.2
#define AUDIT 0.25
#define TIMEOUT 1000 /* feedback timeout, ms */
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

static void inv(const gate *g) {
    char err[160] = "";

    gate_test_checks++;
    if (!gateCheckInvariants(g, err, sizeof(err))) {
        gate_test_failures++;
        printf("  in %s: invariant broken: %s\n", gate_test_current, err);
    }
}

/* Log of the wealth a region of generation 'gen' must reach. */
static double barOf(uint64_t gen, uint32_t tested_rows, uint32_t entries, double delta) {
    return log((double)gen * ((double)gen + 1.0)) +
           log((double)tested_rows * (double)entries) - log(delta);
}

/* Correct answers a region needs to reach 'bar' when it has also seen 'k'
 * wrong ones before getting there: the formula of the file comment. */
static uint32_t needCorrect(double alpha, double bar, uint32_t k) {
    static const double c[4] = {0.1, 0.25, 0.5, 0.9};
    uint32_t n;

    for (n = 1;; n++) {
        double wealth = 0.0;
        int j;

        for (j = 0; j < 4; j++)
            wealth += pow(1.0 + c[j] * alpha / (1.0 - alpha), n) * pow(1.0 - c[j], k);
        if (log(wealth / 4.0) >= bar) return n;
    }
}

/* Number of thresholds at or below 'score': the regions it belongs to. */
static uint32_t regionsOf(const gate *g, double score) {
    uint32_t t, n = 0;

    for (t = 0; t < T; t++)
        if (g->grid[t] <= score) n++;
    return n;
}

/* Spot checks until the drift alarm when their outcomes follow 'pattern'
 * ('x' wrong, '.' right), 0 if it does not fire: the detector of
 * gate_stats.h run by itself. */
static uint32_t alarmAfter(double alpha, const char *pattern) {
    gateDriftBets bets;
    gateDrift d;
    uint32_t i;

    gateDriftBetsInit(&bets, alpha);
    gateDriftInit(&d);
    for (i = 0; pattern[i]; i++)
        if (gateDriftPush(&d, &bets, pattern[i] == 'x', log(GATE_DRIFT_ALARM))) return i + 1;
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

/* GPREDICT on a score gate, the way the module does it: maintain, evaluate,
 * predict. */
static gatePredictResult ask(gate *g, int64_t now, double u, double score) {
    gatePredictResult res;
    gateEval ev;

    gateMaintain(g, now);
    CHECK(gateEvaluate(g, NULL, score, &ev) == GATE_OK);
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
    CHECK(gateEvaluate(g, x, 0.0, &ev) == GATE_OK);
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
    uint32_t i, n = needCorrect(ALPHA, barOf(1, 1, T, DELTA), 0);

    for (i = 0; i < n; i++) feed(g, now, U_PLAIN, 0.9, 1);
    CHECK(g->serving != NULL && g->serving->policy[0] == 0);
    return g;
}

/* A two-label gate whose first snapshot has just been taken after 'n'
 * examples, XA -> label 0 and XB -> label 1 in turn. */
static gate *trainedGate(uint32_t n) {
    gate *g = classGate(2, n);
    uint32_t i;

    for (i = 0; i < n; i++) train(g, T0, (i & 1) ? &XB : &XA, (int32_t)(i & 1));
    CHECK(g->candidate != NULL && g->candidate->gen == 1);
    return g;
}

/* Flags of a waiting request, all ones if it is not waiting. */
static uint32_t flagsOf(const gate *g, uint64_t seq) {
    const gateObs *o = gateFindObservation(g, seq);

    return o ? o->flags : ~0u;
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
    h = mix(h, gen->paired);
    h = mix(h, gen->audits);
    h = mix(h, gen->audit_errors);
    h = mix(h, gen->budget);
    for (i = 0; i < g->nrows * T; i++) {
        const gateRegion *reg = &gen->regions[i];

        for (j = 0; j < GATE_NBETS; j++) h = mixDouble(h, reg->logw[j]);
        h = mix(h, reg->n);
        h = mix(h, reg->errors);
        h = mix(h, reg->certified_at);
    }
    for (r = 0; r < g->nrows; r++) {
        h = mix(h, (uint64_t)(int64_t)gen->policy[r]);
        for (j = 0; j < GATE_DRIFT_ALTS; j++) h = mixDouble(h, gen->drift[r].logw[j]);
        h = mix(h, gen->drift[r].n);
        h = mix(h, gen->drift[r].errors);
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
    h = mix(h, g->next_seq);
    h = mix(h, g->npending);
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
        {0.2, 320.0, 0, 35}, {0.2, 320.0, 1, 47},  {0.2, 320.0, 2, 57}, {0.2, 320.0, 3, 68},
        {0.2, 960.0, 0, 41}, {0.2, 1920.0, 0, 44}, {0.2, 80.0, 0, 28},  {0.2, 80.0, 10, 107},
        {0.2, 240.0, 0, 34}, {0.2, 120.0, 0, 30},  {0.1, 80.0, 0, 60}
    };
    size_t i;

    CHECK_NEAR(barOf(1, 1, 32, 0.2), log(320.0), 1e-12);
    CHECK_NEAR(barOf(3, 1, 32, 0.2), log(1920.0), 1e-12);
    CHECK_NEAR(barOf(2, 2, 4, 0.2), log(240.0), 1e-12);
    CHECK_NEAR(barOf(3, 3, 4, 0.2), log(720.0), 1e-12);
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
    }
    CHECK_EQ_INT(alarmAfter(0.2, "xxxxxxxxxx"), 7);
    /* 4 wrong: 16 and 81. 3 right: 6.75 and 10.125. Then wrong again: the
     * average passes 1000 at the fifth (216 and 2460.4), not at the fourth
     * (108 and 820.1). */
    CHECK_EQ_INT(alarmAfter(0.2, "xxxx...xxxxxxxx"), 12);
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
    CHECK_EQ_INT(cfg.feedback_timeout_ms, 5 * 60 * 1000);

    /* ALPHA has no default: it is the one thing the user must say. */
    CHECK(gateConfigValidate(&cfg, err, sizeof(err)) == GATE_ERR);
    CHECK(err[0] != '\0');
    cfg.alpha = 0.1;
    CHECK(gateConfigValidate(&cfg, err, sizeof(err)) == GATE_OK);
    gateConfigFree(&cfg);
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

/* ------------------------------ Score gate ------------------------------- */

TEST(score_life) {
    const double bar = barOf(1, 1, T, DELTA);
    const uint32_t need = needCorrect(ALPHA, bar, 0);
    gate *g = scoreGate();
    gatePredictResult r, spot, low, top;
    const gateObs *o;
    int64_t now = T0;
    uint32_t i, t;

    /* Nothing to freeze: generation 1 is under test from the first moment. */
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    CHECK(g->candidate != NULL && g->serving == NULL);
    CHECK_EQ_INT(g->candidate->gen, 1);
    CHECK_EQ_INT(g->generations, 1);
    CHECK_EQ_INT(g->candidate->frozen_ms, T0);
    CHECK_EQ_INT(g->candidate->promoted_ms, 0);
    CHECK_EQ_INT(g->candidate->budget, 0);
    CHECK_NEAR(g->candidate->log_threshold, log(320.0), 1e-12);
    CHECK_EQ_INT(g->candidate->policy[0], -1);
    CHECK(g->lineage == LINEAGE && g->epoch == EPOCH);
    CHECK(g->created_ms == T0 && g->now_ms == T0);
    CHECK(g->trainer.w == NULL);
    CHECK(gateRowThreshold(g, 0) < 0.0);
    CHECK(gateCoverage(g) == 0.0);
    CHECK(gateRowAlpha(g, 0) == ALPHA);
    CHECK_EQ_INT(gateRowTested(g, 0), 1);
    CHECK_EQ_INT(gateTestedRows(g), 1);
    CHECK_EQ_INT(gateEntryPoints(g), T);

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
    CHECK_EQ_INT(g->candidate->samples, 3);
    CHECK_EQ_INT(g->candidate->errors, 3);
    for (t = 0; t < T; t++) {
        CHECK_EQ_INT(g->candidate->regions[t].n, t < 4 ? 3 : 0);
        CHECK_EQ_INT(g->candidate->regions[t].errors, t < 4 ? 3 : 0);
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
    CHECK_EQ_INT(g->serving->gen, 1);
    CHECK_EQ_INT(g->generations, 1);
    CHECK_EQ_INT(g->serving->promoted_ms, now);
    CHECK_EQ_INT(g->stats.promotions, 1);
    CHECK_EQ_INT(g->serving->samples, 38);
    CHECK_EQ_INT(g->serving->errors, 3);
    for (t = 0; t < T; t++) {
        const gateRegion *reg = &g->serving->regions[t];

        CHECK_EQ_INT(reg->certified_at, t >= 4 && t < 12 ? 35 : 0);
        CHECK_EQ_INT(reg->n, t < 4 ? 38 : (t < 12 ? 35 : 0));
    }
    CHECK_EQ_INT(g->serving->policy[0], 4);
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
    CHECK_EQ_INT(g->serving->samples, 39);
    CHECK_EQ_INT(g->serving->audits, 1);
    CHECK_EQ_INT(answer(g, now, low.seq, 0), 1);
    CHECK_EQ_INT(g->serving->samples, 40);
    CHECK_EQ_INT(g->serving->errors, 4);
    CHECK_EQ_INT(g->serving->audits, 1);
    CHECK_EQ_INT(g->serving->regions[0].errors, 4);
    CHECK_EQ_INT(g->serving->regions[4].errors, 0);
    CHECK_EQ_INT(answer(g, now, top.seq, 1), 1);
    CHECK_EQ_INT(g->serving->audits, 2);
    CHECK_EQ_INT(g->serving->audit_errors, 0);
    CHECK_EQ_INT(g->serving->regions[T - 1].n, 1); /* 1.0 is in every region */
    CHECK_EQ_INT(g->serving->drift[0].n, 2);
    CHECK_EQ_INT(g->stats.learned, 41);
    CHECK_EQ_INT(g->npending, 0);
    CHECK_EQ_INT(g->serving->policy[0], 4);
    gateFree(g);
}

/* After promotion the lower thresholds keep being tested on the spot
 * checks: the policy moves down as they are proven, and never back up. */
TEST(continued_testing) {
    const double bar = barOf(1, 1, T, DELTA);
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
    CHECK_EQ_INT(g->serving->policy[0], 4);

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
        CHECK_EQ_INT(g->serving->policy[0], i < n1 ? 4 : 1);
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
        CHECK_EQ_INT(g->serving->policy[0], i < n2 ? 1 : 0);
    }
    CHECK(gateRowThreshold(g, 0) == 0.0);
    r = ask(g, now, U_PLAIN, 0.0);
    CHECK(r.action == GATE_ACTION_ACCEPT);
    CHECK_NEAR(gateCoverage(g), 27.0 / 31.0, 1e-12);
    CHECK_EQ_INT(g->serving->regions[4].certified_at, n0);
    CHECK_EQ_INT(g->serving->regions[1].certified_at, n1 + 1);
    CHECK_EQ_INT(g->serving->regions[0].certified_at, n2 + 2);

    /* A proof is "the wealth reached the bar once". Three wrong spot checks
     * take the wealth far below it again; what was proven stays proven. */
    for (i = 0; i < 3; i++) {
        r = ask(g, now, U_SPOT, 0.9);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
    }
    CHECK(!gateRegionCertifies(&g->serving->regions[0], g->serving->log_threshold));
    CHECK_EQ_INT(g->serving->regions[0].certified_at, n2 + 2);
    CHECK_EQ_INT(g->serving->policy[0], 0);
    CHECK_EQ_INT(g->serving->audit_errors, 3);
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
    CHECK(g->serving != NULL && g->serving->policy[0] == 4);
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

    /* Score gate: both generations "guess" the application's score. */
    CHECK(gateEvaluate(g, NULL, 0.625, &ev) == GATE_OK);
    CHECK(ev.has_candidate == 1 && ev.has_serving == 0);
    CHECK(ev.candidate.label == -1 && ev.candidate.score == 0.625);
    for (i = 0; i < 35; i++) feed(g, T0, U_PLAIN, 0.9, 1);
    CHECK(gateEvaluate(g, NULL, 0.25, &ev) == GATE_OK);
    CHECK(ev.has_candidate == 0 && ev.has_serving == 1);
    CHECK(ev.serving.label == -1 && ev.serving.score == 0.25);
    gateFree(g);

    /* Classifier: nothing to guess with until a snapshot exists. */
    g = classGate(2, 12);
    CHECK(gateEvaluate(g, &XA, 0.0, &ev) == GATE_OK);
    CHECK(ev.has_candidate == 0 && ev.has_serving == 0);
    CHECK(gateEvaluate(g, NULL, 0.0, &ev) == GATE_OK);
    for (i = 0; i < 12; i++) train(g, T0, (i & 1) ? &XB : &XA, (int32_t)(i & 1));
    CHECK(gateEvaluate(g, &XB, 0.0, &ev) == GATE_OK);
    CHECK(ev.has_candidate == 1 && ev.has_serving == 0);
    CHECK_EQ_INT(ev.candidate.label, 1);
    CHECK_EQ_INT(gateModelPredict(&g->candidate->model, &XB, NULL, &score), 1);
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
    CHECK_EQ_INT(gateEntryPoints(g), 4);

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
    CHECK_EQ_INT(g->candidate->gen, 1);
    CHECK_EQ_INT(g->generations, 1);
    CHECK_EQ_INT(g->candidate->model.updates, 240);
    CHECK_EQ_INT(g->snapshot_updates, 240);
    CHECK(g->candidate->model.w != NULL && g->candidate->model.w != g->trainer.w);
    /* Two tested labels, four entry points each. */
    CHECK_NEAR(g->candidate->log_threshold, log(80.0), 1e-12);
    /* 20 times what the most demanding label needs with its best bet: the
     * stricter one gains log(1.1) per answer, 4.382 / 0.0953 = 45.98. */
    CHECK_EQ_INT(g->candidate->budget, 20 * 46);

    /* A request asked before the snapshot existed is not proof for it. */
    CHECK_EQ_INT(flagsOf(g, early[3].seq), 0);
    CHECK_EQ_INT(answer(g, now, early[3].seq, 0), 1);
    CHECK_EQ_INT(g->trainer.updates, 241);
    CHECK_EQ_INT(g->candidate->samples, 0);

    /* The frozen model is sure about the three clue vectors: its scores
     * reach at least the lowest entry point (threshold index 7). */
    CHECK_EQ_INT(gateModelPredict(&g->candidate->model, &XA, NULL, &sa), 0);
    CHECK_EQ_INT(gateModelPredict(&g->candidate->model, &XB, NULL, &sb), 1);
    CHECK_EQ_INT(gateModelPredict(&g->candidate->model, &XC, NULL, &sc), 2);
    CHECK(regionsOf(g, sa) >= 8 && regionsOf(g, sb) >= 8 && regionsOf(g, sc) >= 8);

    /* CALIBRATING: every request is proof for the label the snapshot
     * guessed. label0 needs 28 correct answers, label1 needs 60. */
    need_a = needCorrect(ALPHA, log(80.0), 0);
    need_b = needCorrect(0.1, log(80.0), 0);
    CHECK(need_a == 28 && need_b == 60);
    for (i = 1; i < need_a; i++) {
        r = askText(g, now, (i & 1) ? U_SPOT : U_PLAIN, &XA);
        CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0);
        CHECK(flagsOf(g, r.seq) & GATE_OBS_CAND);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
        if (i <= 20) feedText(g, now, U_PLAIN, &XB, 1);
        if (i <= 5) feedText(g, now, U_PLAIN, &XC, 2);
    }
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    CHECK_EQ_INT(g->candidate->samples, 27 + 20 + 5);
    CHECK_EQ_INT(g->candidate->errors, 0);
    for (t = 0; t < T; t++) {
        CHECK_EQ_INT(g->candidate->regions[0 * T + t].n, t < regionsOf(g, sa) ? 27 : 0);
        CHECK_EQ_INT(g->candidate->regions[1 * T + t].n, t < regionsOf(g, sb) ? 20 : 0);
        CHECK_EQ_INT(g->candidate->regions[2 * T + t].n, 0); /* never tested */
    }
    feedText(g, now, U_PLAIN, &XA, 0); /* the 28th */
    CHECK(gateState(g) == GATE_STATE_ACTIVE);
    CHECK(g->serving != NULL && g->serving->gen == 1 && g->candidate == NULL);
    CHECK_EQ_INT(g->stats.promotions, 1);
    /* Every region the score reaches crossed together; from the entry
     * point 7 downwards they are proven. */
    CHECK_EQ_INT(g->serving->policy[0], 0);
    CHECK_EQ_INT(g->serving->policy[1], -1);
    CHECK_EQ_INT(g->serving->policy[2], -1);
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
    CHECK_EQ_INT(g->serving->audits, 1);
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
    CHECK_EQ_INT(g->serving->regions[1 * T].n, 22);
    /* label2 is never answered and never tested: its requests only train. */
    r = askText(g, now, U_PLAIN, &XC);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0);
    CHECK_EQ_INT(flagsOf(g, r.seq), 0);
    CHECK_EQ_INT(answer(g, now, r.seq, 2), 1);
    r = askText(g, now, U_SPOT, &XC);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0);
    CHECK_EQ_INT(flagsOf(g, r.seq), GATE_OBS_AUDIT);
    CHECK_EQ_INT(answer(g, now, r.seq, 2), 1);
    CHECK_EQ_INT(g->serving->samples, 53 + 3);

    /* label1 gets its threshold at its own 60th correct answer. */
    for (i = 23; i <= need_b; i++) {
        feedText(g, now, (i & 1) ? U_SPOT : U_PLAIN, &XB, 1);
        CHECK_EQ_INT(g->serving->policy[1], i < need_b ? -1 : 0);
    }
    r = askText(g, now, U_PLAIN, &XB);
    CHECK(r.action == GATE_ACTION_ACCEPT && r.label == 1 && r.seq == 0);
    r = askText(g, now, U_PLAIN, &XC);
    CHECK(r.action == GATE_ACTION_ABSTAIN);
    CHECK_EQ_INT(answer(g, now, r.seq, 2), 1);
    for (t = 0; t < T; t++) CHECK_EQ_INT(g->serving->regions[2 * T + t].n, 0);
    CHECK_EQ_INT(g->serving->policy[2], -1);
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

    CHECK_EQ_INT(gateModelPredict(&g->candidate->model, &XA, NULL, &score), 0);
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
    CHECK_EQ_INT(g->candidate->samples, 0);

    /* The teacher says label0: right for the guess recorded with the
     * request, although the live model would now say label1. */
    CHECK_EQ_INT(answer(g, now, r1.seq, 0), 1);
    CHECK_EQ_INT(g->candidate->samples, 1);
    CHECK_EQ_INT(g->candidate->errors, 0);
    CHECK_EQ_INT(g->candidate->regions[0 * T].n, 1);
    CHECK_EQ_INT(g->candidate->regions[1 * T].n, 0);
    /* And label1 is wrong for it, although the live model agrees. */
    CHECK_EQ_INT(answer(g, now, r2.seq, 1), 1);
    CHECK_EQ_INT(g->candidate->errors, 1);
    CHECK_EQ_INT(g->candidate->regions[0 * T].errors, 1);
    CHECK_EQ_INT(g->candidate->regions[1 * T].n, 0);
    gateFree(g);
}

TEST(asked_before_snapshot) {
    gate *g = trainedGate(40);
    gatePredictResult before, after, other;
    int64_t now = T0;
    uint32_t i;

    for (i = 0; i < 28; i++) feedText(g, now, U_PLAIN, &XA, 0);
    CHECK(gateState(g) == GATE_STATE_ACTIVE && g->candidate == NULL);

    /* A spot check asked while generation 1 serves alone... */
    before = askText(g, now, U_SPOT, &XA);
    CHECK(before.action == GATE_ACTION_AUDIT);
    CHECK_EQ_INT(flagsOf(g, before.seq), GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_SERV);
    /* ...then the next snapshot is taken (40 examples after the first)... */
    for (i = 0; i < 12; i++) train(g, now, (i & 1) ? &XB : &XA, (int32_t)(i & 1));
    CHECK(g->candidate != NULL && g->candidate->gen == 2);
    /* ...and the answer is proof for generation 1 only. */
    CHECK_EQ_INT(answer(g, now, before.seq, 0), 1);
    CHECK_EQ_INT(g->serving->samples, 29);
    CHECK_EQ_INT(g->candidate->samples, 0);

    /* With something serving, the candidate's proof is the spot checks. */
    after = askText(g, now, U_SPOT, &XA);
    CHECK_EQ_INT(flagsOf(g, after.seq),
                 GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_SERV | GATE_OBS_CAND);
    other = askText(g, now, U_PLAIN, &XB); /* label1: unproven, not a spot check */
    CHECK_EQ_INT(flagsOf(g, other.seq), GATE_OBS_SERV);
    CHECK_EQ_INT(answer(g, now, after.seq, 0), 1);
    CHECK_EQ_INT(answer(g, now, other.seq, 1), 1);
    CHECK_EQ_INT(g->candidate->samples, 1);
    CHECK_EQ_INT(g->serving->samples, 31);
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
    CHECK_EQ_INT(g->serving->samples, 35);
    /* Asked of the candidate, answered when it already serves. */
    for (i = 35; i < 40; i++) CHECK_EQ_INT(answer(g, now, r[i].seq, 1), 1);
    CHECK_EQ_INT(g->serving->samples, 40);
    CHECK_EQ_INT(g->serving->regions[0].n, 40);
    CHECK_EQ_INT(g->serving->audits, 0);
    gateFree(g);
}

/* --------------------------------- Order --------------------------------- */

TEST(answers_wait_for_their_turn) {
    gate *g = scoreGate();
    gatePredictResult r[40], early, proof;
    int64_t now = T0;
    uint32_t i;

    for (i = 0; i < 40; i++) r[i] = ask(g, now, U_PLAIN, 0.9);
    /* Newest first: each answer is taken, none is applied. */
    for (i = 39; i >= 1; i--) {
        CHECK_EQ_INT(answer(g, now, r[i].seq, 1), 1);
        CHECK(flagsOf(g, r[i].seq) & GATE_OBS_RESOLVED);
        CHECK_EQ_INT(g->candidate->samples, 0);
        CHECK_EQ_INT(g->npending, 40);
    }
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    /* The oldest arrives and all 40 are applied, in request order: the
     * 35th proved the threshold. */
    CHECK_EQ_INT(answer(g, now, r[0].seq, 1), 1);
    CHECK(gateState(g) == GATE_STATE_ACTIVE);
    CHECK_EQ_INT(g->serving->samples, 40);
    CHECK_EQ_INT(g->serving->regions[0].certified_at, 35);
    CHECK_EQ_INT(g->npending, 0);
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
    CHECK_EQ_INT(g->candidate->samples, 1);
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
    int i;

    ga[0] = a->candidate;
    ga[1] = a->serving;
    gb[0] = b->candidate;
    gb[1] = b->serving;
    CHECK(gateState(a) == gateState(b));
    CHECK_EQ_INT(a->generations, b->generations);
    CHECK_EQ_INT(a->npending, b->npending);
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
        CHECK_EQ_INT(ga[i]->paired, gb[i]->paired);
        CHECK_EQ_INT(ga[i]->audits, gb[i]->audits);
        CHECK_EQ_INT(ga[i]->audit_errors, gb[i]->audit_errors);
        CHECK(memcmp(ga[i]->policy, gb[i]->policy, sizeof(int32_t) * a->nrows) == 0);
        /* Wealth, counts and the sample at which each region crossed. */
        CHECK(memcmp(ga[i]->regions, gb[i]->regions, sizeof(gateRegion) * a->nrows * T) == 0);
        CHECK(memcmp(ga[i]->drift, gb[i]->drift, sizeof(gateDrift) * a->nrows) == 0);
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
    enum { N = 80, PHASES = 14 };
    gate *g[3];
    uint64_t rng = 20261128, order_rng = 7;
    int64_t now = T0;
    int phase, k;
    uint32_t i;

    for (k = 0; k < 3; k++) g[k] = scoreGate();
    for (phase = 0; phase < PHASES; phase++) {
        double score[N], u[N];
        int32_t result[N];
        int lost[N];
        uint64_t seq[3][N];

        for (i = 0; i < N; i++) {
            /* The scorer is broken for a while in the middle. */
            double right = phase >= 6 && phase <= 8 ? 0.2 : 0.97;

            score[i] = gate_test_uniform(&rng);
            u[i] = gate_test_uniform(&rng);
            result[i] = gate_test_uniform(&rng) < (score[i] >= 0.5 ? right : 0.5);
            lost[i] = gate_test_uniform(&rng) < 0.04;
        }
        /* The same requests, asked in the same state: same replies. */
        for (k = 0; k < 3; k++) {
            for (i = 0; i < N; i++) {
                gatePredictResult r = ask(g[k], now, u[i], score[i]);

                seq[k][i] = r.seq;
                CHECK(seq[k][i] == seq[0][i]);
            }
        }
        for (k = 0; k < 3; k++) answerBatch(g[k], k, now, seq[k], result, lost, N, &order_rng);
        /* The lost answers time out. Whatever the order was, the proof is
         * the same down to the last bit. */
        now += TIMEOUT;
        for (k = 0; k < 3; k++) {
            gateMaintain(g[k], now);
            inv(g[k]);
            CHECK_EQ_INT(g[k]->npending, 0);
        }
        sameProof(g[0], g[1]);
        sameProof(g[0], g[2]);
        CHECK_EQ_INT(g[0]->stats.missing, g[1]->stats.missing);
        CHECK_EQ_INT(g[0]->stats.missing, g[2]->stats.missing);
    }
    /* The phases went through lost answers, a promotion, a drift alarm and
     * the promotion of the generation after it. */
    CHECK(g[0]->stats.missing >= 1);
    CHECK_EQ_INT(g[0]->stats.revocations, 1);
    CHECK_EQ_INT(g[0]->stats.promotions, 2);
    CHECK(g[0]->serving != NULL && g[0]->serving->gen == 2);
    for (k = 0; k < 3; k++) gateFree(g[k]);
}

/* The same for a classifier, with hand-made guesses for both generations so
 * that the three gates see the same guesses although their live models,
 * trained in three different orders, are no longer the same. */
TEST(answer_order_does_not_matter_classifier) {
    enum { N = 100, PHASES = 10 };
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
        int32_t slabel[N], clabel[N], result[N];
        double sscore[N], cscore[N], u[N];
        int lost[N];
        uint64_t seq[3][N];

        for (i = 0; i < N; i++) {
            clabel[i] = (int32_t)(gate_test_rand64(&rng) & 1);
            slabel[i] = gate_test_uniform(&rng) < 0.9 ? clabel[i] : 1 - clabel[i];
            cscore[i] = scores[gate_test_rand64(&rng) % COUNTOF(scores)];
            /* The serving generation is the less confident one, so that a
             * candidate can come to accept more than it does. Half of the
             * requests are spot checks. */
            sscore[i] = scores[gate_test_rand64(&rng) % 3];
            u[i] = 0.5 * gate_test_uniform(&rng);
            result[i] = gate_test_uniform(&rng) < (cscore[i] >= 0.9 ? 0.97 : 0.6)
                            ? clabel[i] : 1 - clabel[i];
            lost[i] = gate_test_uniform(&rng) < 0.04;
        }
        for (k = 0; k < 3; k++) {
            for (i = 0; i < N; i++) {
                gateEval ev = guesses(g[k], slabel[i], sscore[i], clabel[i], cscore[i]);
                gatePredictResult r = askGuess(g[k], now, u[i], &ev, &XA);

                seq[k][i] = r.seq;
                CHECK(seq[k][i] == seq[0][i]);
            }
        }
        for (k = 0; k < 3; k++) answerBatch(g[k], k, now, seq[k], result, lost, N, &order_rng);
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
    CHECK(g[0]->stats.promotions >= 2); /* a first snapshot, then a better one */
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
    CHECK_EQ_INT(g->candidate->samples, 0);
    CHECK_EQ_INT(answer(g, now, r2.seq, 1), 0);
    CHECK_EQ_INT(g->trainer.updates, 5);
    CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
    CHECK_EQ_INT(g->candidate->samples, 2);
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

/* ----------------------------- Missing answers --------------------------- */

TEST(missing_answers) {
    gate *g = scoreGate();
    gatePredictResult lost, held;
    gateGeneration *cand = g->candidate;
    int64_t now = T0;
    uint32_t i;

    for (i = 0; i < 34; i++) feed(g, now, U_PLAIN, 0.9, 1);
    /* One answer short of the proof. The next request never gets its
     * answer; the one after it does, and waits behind it. */
    lost = ask(g, now, U_PLAIN, 0.9);
    held = ask(g, now + 10, U_PLAIN, 0.9);
    CHECK_EQ_INT(answer(g, now + 20, held.seq, 1), 1);
    CHECK_EQ_INT(g->candidate->samples, 34);
    CHECK_EQ_INT(gateMaintenanceDue(g, now + TIMEOUT - 1), 0);
    gateMaintain(g, now + TIMEOUT - 1);
    CHECK_EQ_INT(g->npending, 2);
    CHECK_EQ_INT(gateMaintenanceDue(g, now + TIMEOUT), 1);

    /* When the window closes the lost answer counts as a wrong guess, then
     * the held one is applied. Had the held one gone first it would have
     * been correct answer 35 and the proof. */
    gateMaintain(g, now + TIMEOUT);
    inv(g);
    CHECK_EQ_INT(g->npending, 0);
    CHECK_EQ_INT(g->stats.missing, 1);
    CHECK_EQ_INT(g->stats.dropped, 0);
    CHECK(g->candidate == cand && g->serving == NULL); /* still the candidate */
    CHECK_EQ_INT(g->candidate->samples, 36);
    CHECK_EQ_INT(g->candidate->errors, 1);
    CHECK_EQ_INT(g->candidate->regions[0].n, 36);
    CHECK_EQ_INT(g->candidate->regions[0].errors, 1);
    CHECK_EQ_INT(g->candidate->regions[12].n, 0);
    CHECK_EQ_INT(answer(g, now + TIMEOUT, lost.seq, 1), 0); /* too late */
    CHECK_EQ_INT(g->stats.learned, 35);

    /* Lost answers cost wealth and nothing else: a score gate's candidate
     * is still there after hundreds of them. */
    now += 2 * TIMEOUT;
    for (i = 0; i < 300; i++) CHECK(ask(g, now, U_PLAIN, 0.9).seq != 0);
    gateMaintain(g, now + TIMEOUT);
    inv(g);
    CHECK(g->candidate == cand && g->candidate->gen == 1);
    CHECK_EQ_INT(g->candidate->samples, 336);
    CHECK_EQ_INT(g->candidate->errors, 301);
    CHECK_EQ_INT(g->stats.missing, 301);
    CHECK_EQ_INT(g->stats.retired + g->stats.revocations, 0);
    gateFree(g);

    /* A request that was only wanted for training is simply forgotten. */
    g = classGate(2, 12);
    CHECK(askText(g, now, U_PLAIN, &XA).seq != 0);
    CHECK(askText(g, now, U_SPOT, &XB).seq != 0);
    CHECK_EQ_INT(gateMaintenanceDue(g, now + TIMEOUT), 1);
    gateMaintain(g, now + TIMEOUT);
    inv(g);
    CHECK_EQ_INT(g->npending, 0);
    CHECK_EQ_INT(g->pending_bytes, 0);
    CHECK_EQ_INT(g->stats.dropped, 2);
    CHECK_EQ_INT(g->stats.missing, 0);
    CHECK_EQ_INT(g->trainer.updates, 0);
    gateFree(g);
}

/* "Applied as an error", exactly: a gate whose teacher never answers some
 * requests ends where a gate ends whose teacher answers them all wrong.
 * That includes the spot checks of answers, which feed the drift detector. */
TEST(missing_is_a_wrong_answer) {
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
    for (k = 0; k < 2; k++) gateMaintain(g[k], now);
    sameProof(wrong, lost);
    CHECK_EQ_INT(lost->candidate->samples, 40);
    CHECK_EQ_INT(lost->candidate->errors, 2);
    CHECK_EQ_INT(lost->stats.missing, 2);
    CHECK_EQ_INT(wrong->stats.missing, 0);

    /* With two wrong answers the proof takes 57 correct ones: 19 more. */
    for (k = 0; k < 2; k++) {
        for (i = 0; i < 19; i++) {
            CHECK(gateState(g[k]) == GATE_STATE_CALIBRATING);
            feed(g[k], now, U_PLAIN, 0.9, 1);
        }
        CHECK(gateState(g[k]) == GATE_STATE_ACTIVE);
    }

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
    sameProof(wrong, lost);
    CHECK_EQ_INT(lost->serving->audits, 6);
    CHECK_EQ_INT(lost->serving->audit_errors, 2);
    CHECK_EQ_INT(lost->serving->drift[0].errors, 2);
    CHECK_EQ_INT(lost->stats.missing, 4);
    gateFree(wrong);
    gateFree(lost);
}

/* ------------------------- Entry points and the bar ---------------------- */

#define B(t) (1u << (t))

static uint32_t span(uint32_t lo, uint32_t hi) {
    uint32_t t, mask = 0;

    for (t = lo; t <= hi; t++) mask |= B(t);
    return mask;
}

/* The policy the gate derives for a row whose crossed regions are 'mask'. */
static int32_t policyOf(const gate *g, gateGeneration *gen, uint32_t row, uint32_t mask) {
    uint32_t t;

    for (t = 0; t < T; t++) gen->regions[row * T + t].certified_at = (mask >> t) & 1;
    gateGenerationRefreshPolicy(g, gen, row);
    return gen->policy[row];
}

/* The definition, spelled out: a region is proven when it and every region
 * between it and some entry point at or above it have crossed. The policy
 * is the lowest proven region. */
static int32_t policyByDefinition(uint32_t mask, int classifier) {
    int32_t t, e;

    for (t = 0; t < (int32_t)T; t++) {
        for (e = t; e < (int32_t)T && ((mask >> e) & 1); e++)
            if (!classifier || e == 7 || e == 15 || e == 23 || e == 31) return t;
    }
    return -1;
}

TEST(entry_points) {
    gateConfig cfg;
    gateGeneration *gen;
    uint64_t rng = 99;
    uint32_t i, t, r;
    gate *g;

    classConfig(&cfg, 3, 10);
    cfg.class_defer[2] = 1;
    g = create(&cfg);
    gen = gateGenerationAllocate(g);
    if (gen == NULL) die("out of memory");

    /* An empty generation: no proof, no policy, nothing frozen. */
    CHECK(gen->gen == 0 && gen->samples == 0 && gen->errors == 0 && gen->paired == 0 &&
          gen->audits == 0 && gen->audit_errors == 0 && gen->budget == 0 &&
          gen->frozen_ms == 0 && gen->promoted_ms == 0 && gen->model.w == NULL);
    for (i = 0; i < 3 * T; i++) {
        const gateRegion *reg = &gen->regions[i];

        CHECK(reg->n == 0 && reg->errors == 0 && reg->certified_at == 0 &&
              gateRegionLogWealth(reg) == 0.0);
    }
    for (r = 0; r < 3; r++)
        CHECK(gen->policy[r] == -1 && gen->drift[r].n == 0 && gateDriftLogStat(&gen->drift[r]) == 0.0);

    /* Classifier: from the most confident threshold down, with an entry
     * point at every eighth. */
    for (t = 0; t < T; t++) CHECK_EQ_INT(gateIsEntry(g, t), t == 7 || t == 15 || t == 23 || t == 31);
    CHECK_EQ_INT(gateEntryPoints(g), 4);
    CHECK_EQ_INT(gateTestedRows(g), 2);
    CHECK_EQ_INT(policyOf(g, gen, 0, 0), -1);
    CHECK_EQ_INT(policyOf(g, gen, 0, B(31)), 31);
    CHECK_EQ_INT(policyOf(g, gen, 0, B(30)), -1);          /* nothing above it is proven */
    CHECK_EQ_INT(policyOf(g, gen, 0, span(24, 30)), -1);
    CHECK_EQ_INT(policyOf(g, gen, 0, span(29, 31)), 29);
    CHECK_EQ_INT(policyOf(g, gen, 0, B(31) | B(29)), 31);  /* 30 breaks the chain */
    CHECK_EQ_INT(policyOf(g, gen, 0, B(23)), 23);
    CHECK_EQ_INT(policyOf(g, gen, 0, B(15)), 15);
    CHECK_EQ_INT(policyOf(g, gen, 0, B(7)), 7);
    CHECK_EQ_INT(policyOf(g, gen, 0, B(22)), -1);
    CHECK_EQ_INT(policyOf(g, gen, 0, B(23) | B(22)), 22);
    CHECK_EQ_INT(policyOf(g, gen, 0, span(0, 6)), -1);
    CHECK_EQ_INT(policyOf(g, gen, 0, span(0, 7)), 0);
    CHECK_EQ_INT(policyOf(g, gen, 0, B(8) | B(7)), 7);     /* 8 is below unproven 9 */
    CHECK_EQ_INT(policyOf(g, gen, 0, B(7) | B(6) | B(4)), 6);
    CHECK_EQ_INT(policyOf(g, gen, 0, B(31) | span(0, 7)), 0);
    CHECK_EQ_INT(policyOf(g, gen, 0, span(16, 31)), 16);
    CHECK_EQ_INT(policyOf(g, gen, 0, span(16, 31) & ~B(24)), 16); /* 23 starts again */
    CHECK_EQ_INT(policyOf(g, gen, 0, span(16, 31) & ~B(23)), 24);
    CHECK_EQ_INT(policyOf(g, gen, 0, 0xffffffffu), 0);
    /* One row at a time. */
    CHECK_EQ_INT(policyOf(g, gen, 1, B(15)), 15);
    CHECK_EQ_INT(gen->policy[0], 0);
    CHECK_EQ_INT(policyOf(g, gen, 0, 0), -1);
    CHECK_EQ_INT(gen->policy[1], 15);
    for (i = 0; i < 20000; i++) {
        uint32_t mask = (uint32_t)gate_test_rand64(&rng);

        /* Sparse and dense patterns alike. */
        if (i % 3 == 1) mask &= (uint32_t)gate_test_rand64(&rng);
        if (i % 3 == 2) mask |= (uint32_t)gate_test_rand64(&rng);
        if (policyOf(g, gen, 0, mask) != policyByDefinition(mask, 1)) break;
    }
    CHECK_EQ_INT(i, 20000);
    gateGenerationRelease(gen);
    gateFree(g);

    /* Score gate: every threshold stands on its own. */
    g = scoreGate();
    gen = gateGenerationAllocate(g);
    if (gen == NULL) die("out of memory");
    for (t = 0; t < T; t++) CHECK_EQ_INT(gateIsEntry(g, t), 1);
    CHECK_EQ_INT(policyOf(g, gen, 0, 0), -1);
    CHECK_EQ_INT(policyOf(g, gen, 0, B(30)), 30);
    CHECK_EQ_INT(policyOf(g, gen, 0, B(20) | B(5)), 5);
    CHECK_EQ_INT(policyOf(g, gen, 0, B(0)), 0);
    for (i = 0; i < 2000; i++) {
        uint32_t mask = (uint32_t)gate_test_rand64(&rng) & (uint32_t)gate_test_rand64(&rng);

        if (policyOf(g, gen, 0, mask) != policyByDefinition(mask, 0)) break;
    }
    CHECK_EQ_INT(i, 2000);
    gateGenerationRelease(gen);
    gateFree(g);
}

/* A live gate obeys the entry points too: a snapshot whose scores never
 * reach threshold index 7 crosses its low regions and proves nothing. */
TEST(crossed_is_not_proven) {
    gate *g = classGate(2, 3);
    gatePredictResult r;
    int64_t now = T0;
    uint32_t i, t;

    for (i = 0; i < 3; i++) train(g, now, &XA, 0);
    CHECK_EQ_INT(regionsOf(g, 0.87), 7); /* just under threshold 7 (0.877) */
    for (i = 0; i < 100; i++) {
        gateEval ev = guesses(g, -1, 0.0, 0, 0.87);

        r = askGuess(g, now, U_PLAIN, &ev, &XA);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
    }
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    for (t = 0; t < T; t++)
        CHECK_EQ_INT(g->candidate->regions[t].certified_at, t < 7 ? 28 : 0);
    CHECK_EQ_INT(g->candidate->policy[0], -1);
    /* One level higher and the same proof counts at once. */
    for (i = 1; i <= 28; i++) {
        gateEval ev = guesses(g, -1, 0.0, 0, 0.88);

        r = askGuess(g, now, U_PLAIN, &ev, &XA);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
        CHECK((g->serving != NULL) == (i == 28));
    }
    CHECK_EQ_INT(g->serving->policy[0], 0);
    gateFree(g);
}

TEST(bar_per_generation) {
    gateConfig cfg;
    gate *g = scoreGate();
    int64_t now = T0;
    uint32_t i;

    /* Score gate: one row, 32 entry points. A reset starts the next
     * generation; the budget of the earlier ones is gone for good. */
    CHECK_NEAR(g->candidate->log_threshold, log(2.0 * 32.0 / 0.2), 1e-12);
    CHECK_NEAR(gateBudgetSpentNow(g), DELTA * 1.0 / 2.0, 1e-15);
    gateReset(g, now, EPOCH2);
    CHECK_EQ_INT(g->candidate->gen, 2);
    CHECK_NEAR(g->candidate->log_threshold, log(6.0 * 32.0 / 0.2), 1e-12);
    CHECK_NEAR(gateBudgetSpentNow(g), DELTA * 2.0 / 3.0, 1e-15);
    gateReset(g, now, EPOCH3);
    CHECK_EQ_INT(g->candidate->gen, 3);
    CHECK_NEAR(g->candidate->log_threshold, log(12.0 * 32.0 / 0.2), 1e-12);
    CHECK_NEAR(gateBudgetSpentNow(g), DELTA * 3.0 / 4.0, 1e-15);
    gateFree(g);

    baseConfig(&cfg);
    cfg.delta = 0.01;
    g = create(&cfg);
    CHECK_NEAR(g->candidate->log_threshold, log(2.0 * 32.0 / 0.01), 1e-12);
    gateFree(g);

    /* Classifier: tested labels times 4 entry points. A recovery keeps the
     * model, so each one freezes the next generation at once. The budget
     * of a candidate is 20 times ceil(bar / log(1.225)). */
    g = classGate(3, 3);
    CHECK_NEAR(gateBudgetSpentNow(g), 0.0, 0.0);
    for (i = 0; i < 3; i++) train(g, now, &XA, 0);
    CHECK_EQ_INT(g->candidate->gen, 1);
    CHECK_NEAR(g->candidate->log_threshold, log(2.0 * 12.0 / 0.2), 1e-12);
    CHECK_EQ_INT(g->candidate->budget, 20 * 24); /* 4.787 / 0.2029 = 23.6 */
    gateRecover(g, now, EPOCH2);
    CHECK_EQ_INT(g->candidate->gen, 2);
    CHECK_NEAR(g->candidate->log_threshold, log(6.0 * 12.0 / 0.2), 1e-12);
    CHECK_EQ_INT(g->candidate->budget, 20 * 30); /* 5.886 / 0.2029 = 29.004 */
    gateRecover(g, now, EPOCH3);
    CHECK_EQ_INT(g->candidate->gen, 3);
    CHECK_NEAR(g->candidate->log_threshold, log(12.0 * 12.0 / 0.2), 1e-12);
    inv(g);
    gateFree(g);

    /* An ALWAYSDEFER label is not tested and takes no share. */
    classConfig(&cfg, 3, 3);
    cfg.class_defer[1] = 1;
    g = create(&cfg);
    for (i = 0; i < 3; i++) train(g, now, &XA, 0);
    CHECK_NEAR(g->candidate->log_threshold, log(2.0 * 8.0 / 0.2), 1e-12);
    gateFree(g);
}

/* -------------------------------- Replacing ------------------------------ */

TEST(replacing_needs_more_accepts) {
    gate *g = classGate(2, 5);
    gatePredictResult r;
    int64_t now = T0;
    gateEval ev;
    uint32_t i;

    for (i = 0; i < 5; i++) train(g, now, &XA, 0);
    CHECK_EQ_INT(g->candidate->budget, 20 * 22); /* 4.382 / 0.2029 = 21.6 */

    /* Generation 1 guesses label0. Ten wrong guesses at 0.6 put its
     * regions 0 and 1 out of reach; 28 correct ones at 0.99 prove the
     * regions 2..15, so it answers from threshold 2 (0.665). */
    CHECK_EQ_INT(regionsOf(g, 0.6), 2);
    CHECK_EQ_INT(regionsOf(g, 0.99), 20);
    for (i = 0; i < 10; i++) {
        ev = guesses(g, -1, 0.0, 0, 0.6);
        r = askGuess(g, now, U_PLAIN, &ev, &XA);
        CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
    }
    for (i = 1; i <= 28; i++) {
        ev = guesses(g, -1, 0.0, 0, 0.99);
        r = askGuess(g, now, U_PLAIN, &ev, &XA);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
        CHECK((g->serving != NULL) == (i == 28));
    }
    CHECK_EQ_INT(g->serving->policy[0], 2);
    /* 38 answers were learned since the first snapshot: the next one is
     * taken in the same breath. */
    CHECK(g->candidate != NULL && g->candidate->gen == 2);
    CHECK_NEAR(g->candidate->log_threshold, log(240.0), 1e-12);
    CHECK_EQ_INT(g->candidate->budget, 20 * 28); /* 5.481 / 0.2029 = 27.006 */

    /* 34 spot checks that both generations would answer, all correct.
     * Generation 2 now has proven thresholds of its own (from 0.5), but it
     * accepted exactly the requests generation 1 accepted. */
    for (i = 1; i <= 34; i++) {
        ev = guesses(g, 0, 0.99, 0, 0.99);
        r = askGuess(g, now, U_SPOT, &ev, &XA);
        CHECK(r.action == GATE_ACTION_AUDIT);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
        CHECK_EQ_INT(g->candidate->policy[0], i < 34 ? -1 : 0);
    }
    CHECK_EQ_INT(g->candidate->paired, 34);
    CHECK_EQ_INT(g->candidate->regions[0].n, 34);
    CHECK(g->serving->gen == 1 && g->candidate->gen == 2);

    /* One that generation 1 would answer and generation 2 would not (it
     * guesses label1, where it has proven nothing): one behind. */
    ev = guesses(g, 0, 0.99, 1, 0.7);
    r = askGuess(g, now, U_SPOT, &ev, &XA);
    CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
    CHECK_EQ_INT(g->candidate->paired, 35);
    CHECK(g->serving->gen == 1);
    /* One that only generation 2 would answer (0.6 is under generation
     * 1's threshold): level. Level is not more. */
    ev = guesses(g, 0, 0.6, 0, 0.99);
    r = askGuess(g, now, U_SPOT, &ev, &XA);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq != 0);
    CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
    CHECK_EQ_INT(g->candidate->regions[0].n, 35);
    CHECK_EQ_INT(g->candidate->paired, 35);
    CHECK(g->serving->gen == 1 && g->serving->policy[0] == 2);
    /* Another one: generation 2 takes over with this very answer. */
    r = askGuess(g, now, U_SPOT, &ev, &XA);
    CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
    CHECK_EQ_INT(g->serving->gen, 2);
    CHECK_EQ_INT(g->serving->policy[0], 0);
    CHECK_EQ_INT(g->serving->paired, 0);
    CHECK_EQ_INT(g->serving->budget, 0);
    CHECK_EQ_INT(g->serving->promoted_ms, now);
    CHECK_EQ_INT(g->stats.promotions, 2);
    CHECK(g->candidate != NULL && g->candidate->gen == 3);
    gateFree(g);
}

TEST(candidate_budget) {
    gate *g = trainedGate(500);
    gatePredictResult r, late;
    int64_t now = T0;
    uint32_t i;

    /* The snapshot says label0 for XA and the teacher says label1 every
     * time. At 440 answers (20 x 22) without a proven threshold it goes. */
    CHECK_EQ_INT(g->candidate->budget, 440);
    for (i = 1; i < 440; i++) {
        r = askText(g, now, U_PLAIN, &XA);
        CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
    }
    CHECK(g->candidate != NULL && g->candidate->samples == 439 && g->candidate->errors == 439);
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

    /* The next snapshot comes 500 examples after the previous one: 441
     * answers were learned since, 59 to go. */
    CHECK_EQ_INT(g->trainer.updates, 941);
    for (i = 0; i < 58; i++) train(g, now, &XB, 1);
    CHECK(g->candidate == NULL);
    train(g, now, &XB, 1);
    CHECK(g->candidate != NULL && g->candidate->gen == 2);
    CHECK_EQ_INT(g->candidate->budget, 560);
    gateFree(g);

    /* With a short step the examples are already there when the budget
     * runs out, and the next snapshot replaces the dropped one at once. */
    g = trainedGate(6);
    for (i = 1; i <= 440; i++) {
        r = askText(g, now, U_PLAIN, &XA);
        CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
        CHECK_EQ_INT(g->candidate->gen, i < 440 ? 1 : 2);
    }
    CHECK_EQ_INT(g->stats.retired, 1);
    CHECK_EQ_INT(g->candidate->samples, 0);
    gateFree(g);

    /* Lost answers use the budget up like wrong ones: 439 leave the
     * snapshot in place, the 440th is its last. Nothing was learned
     * meanwhile, so no new snapshot follows. */
    g = trainedGate(500);
    for (i = 0; i < 439; i++) CHECK(askText(g, now, U_PLAIN, &XA).seq != 0);
    now += TIMEOUT;
    gateMaintain(g, now);
    inv(g);
    CHECK(g->candidate != NULL && g->candidate->samples == 439);
    CHECK_EQ_INT(g->stats.missing, 439);
    CHECK(askText(g, now, U_PLAIN, &XA).seq != 0);
    now += TIMEOUT;
    gateMaintain(g, now);
    inv(g);
    CHECK(g->candidate == NULL);
    CHECK_EQ_INT(g->stats.retired, 1);
    CHECK_EQ_INT(g->trainer.updates, 500);
    CHECK(gateState(g) == GATE_STATE_COLD);
    gateFree(g);
}

TEST(score_candidate_has_no_budget) {
    gate *g = scoreGate();
    uint32_t i;

    CHECK_EQ_INT(g->candidate->budget, 0);
    for (i = 0; i < 2000; i++) feed(g, T0, U_PLAIN, 1.0, 0);
    CHECK(g->candidate != NULL && g->candidate->gen == 1);
    CHECK_EQ_INT(g->candidate->samples, 2000);
    CHECK_EQ_INT(g->stats.retired, 0);
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    gateFree(g);
}

/* ---------------------------------- Drift -------------------------------- */

TEST(drift_score) {
    static const char pattern[] = "xxxx...xxxxx";
    int64_t now = T0;
    gate *g = activeScoreGate(now);
    gatePredictResult r, waiting;
    uint32_t i;

    /* Seven wrong spot checks in a row (file comment): the gate answers
     * until the seventh arrives. */
    CHECK_EQ_INT(alarmAfter(ALPHA, "xxxxxxx"), 7);
    waiting = ask(g, now, U_SPOT, 0.9);
    for (i = 1; i <= 7; i++) {
        /* The oldest spot check stays open; each wrong answer is for the
         * request before the newest, so six are applied before the last. */
        r = waiting;
        waiting = ask(g, now, U_SPOT, 0.9);
        CHECK(waiting.action == GATE_ACTION_AUDIT);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
        if (i < 7) {
            CHECK(gateState(g) == GATE_STATE_ACTIVE);
            CHECK_EQ_INT(g->serving->audits, i);
            CHECK_EQ_INT(g->serving->audit_errors, i);
            CHECK(ask(g, now, U_PLAIN, 0.9).action == GATE_ACTION_ACCEPT);
        }
    }
    /* Alarm: the generation is gone and, a score gate having nothing to
     * relearn, the next one is under test already. */
    CHECK(g->serving == NULL);
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    CHECK(g->candidate != NULL && g->candidate->gen == 2);
    CHECK_EQ_INT(g->generations, 2);
    CHECK_EQ_INT(g->candidate->samples, 0);
    CHECK_NEAR(g->candidate->log_threshold, log(960.0), 1e-12);
    CHECK_EQ_INT(g->stats.revocations, 1);
    CHECK(gateCoverage(g) == 0.0);
    CHECK(ask(g, now, U_PLAIN, 0.9).action == GATE_ACTION_ABSTAIN);
    /* The spot check that was still open was proof for a generation that
     * no longer exists: its id is void. */
    CHECK(gateFindObservation(g, waiting.seq) == NULL);
    CHECK_EQ_INT(answer(g, now, waiting.seq, 1), 0);
    CHECK_EQ_INT(g->stats.dropped, 1);
    gateFree(g);

    /* Correct spot checks in between only delay it: 12 by the detector's
     * own arithmetic (see expected_numbers). */
    g = activeScoreGate(now);
    CHECK_EQ_INT(alarmAfter(ALPHA, pattern), 12);
    for (i = 1; i <= 12; i++) {
        r = ask(g, now, U_SPOT, 0.9);
        CHECK_EQ_INT(answer(g, now, r.seq, pattern[i - 1] == '.'), 1);
        CHECK((g->serving != NULL) == (i < 12));
    }
    CHECK_EQ_INT(g->stats.revocations, 1);
    /* Only spot checks of answers count. Wrong proof from below the
     * threshold, however much, is not drift. */
    gateFree(g);
    g = scoreGate();
    feed(g, now, U_PLAIN, 0.5, 0);
    for (i = 0; i < 35; i++) feed(g, now, U_PLAIN, 0.9, 1);
    CHECK_EQ_INT(g->serving->policy[0], 4);
    for (i = 0; i < 50; i++) {
        r = ask(g, now, U_SPOT, 0.5);
        CHECK(r.action == GATE_ACTION_ABSTAIN);
        CHECK_EQ_INT(answer(g, now, r.seq, 0), 1);
    }
    CHECK(g->serving != NULL && g->serving->audits == 0 && g->serving->drift[0].n == 0);
    CHECK_EQ_INT(g->serving->errors, 51);
    gateFree(g);
}

TEST(drift_classifier) {
    gate *g = trainedGate(12);
    gatePredictResult r;
    int64_t now = T0;
    uint32_t i, nonzero = 0;

    for (i = 0; i < 28; i++) feedText(g, now, U_PLAIN, &XA, 0);
    CHECK(gateState(g) == GATE_STATE_ACTIVE);
    CHECK(g->serving->gen == 1 && g->serving->policy[0] == 0);
    CHECK(g->candidate != NULL && g->candidate->gen == 2);
    CHECK_EQ_INT(g->trainer.updates, 40);

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
    CHECK(g->candidate != NULL && g->candidate->gen == 3);
    CHECK_EQ_INT(g->generations, 3);
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

TEST(full_by_bytes) {
    /* The largest clue vector there is: all 8192 buckets. */
    const uint64_t per = sizeof(gateObs) + (uint64_t)GATE_DIM * (sizeof(uint32_t) + sizeof(float));
    const uint32_t fit = (uint32_t)((GATE_MAX_PENDING_BYTES + per - 1) / per);
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

    /* 16 MiB of stored clues is 256 such requests, far below 4096. */
    CHECK_EQ_INT(fit, 256);
    for (i = 1; i <= fit; i++) {
        r = askText(g, now, U_PLAIN, &big);
        CHECK(r.seq != 0);
        CHECK_EQ_INT(gateFull(g), i == fit);
        if (i == 1) first = r.seq;
    }
    CHECK_EQ_INT(g->npending, fit);
    CHECK_EQ_INT(g->pending_bytes, fit * per);
    CHECK_EQ_INT(gateMemoryUsage(g), expectBytes(g));
    r = askText(g, now, U_PLAIN, &XA);
    CHECK(r.action == GATE_ACTION_ABSTAIN && r.seq == 0);
    CHECK_EQ_INT(g->stats.full, 1);
    /* An answer releases the clues it came for. */
    CHECK_EQ_INT(answer(g, now, first, 0), 1);
    CHECK(!gateFull(g));
    CHECK_EQ_INT(g->pending_bytes, (fit - 1) * per);
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
    CHECK(g->serving != NULL && g->serving->policy[0] == 0);
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
    CHECK_EQ_INT(g->serving->audits, 1);
    CHECK_EQ_INT(g->serving->audit_errors, 1);
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

    for (i = 0; i < 28; i++) feedText(g, now, U_PLAIN, &XA, 0);
    CHECK(g->serving != NULL && g->serving->gen == 1);
    CHECK(g->candidate != NULL && g->candidate->gen == 2);
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

    gateReset(g, now - 400, EPOCH2); /* an older clock does not turn it back */
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
    /* Kept: configuration, identity, clock, id counter and the ledger. */
    CHECK_EQ_INT(g->generations, 2);
    CHECK_NEAR(gateBudgetSpentNow(g), DELTA * 2.0 / 3.0, 1e-15);
    CHECK(g->lineage == LINEAGE && g->epoch == EPOCH2);
    CHECK(g->created_ms == T0 && g->now_ms == now);
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
    CHECK(g->candidate != NULL && g->candidate->gen == 3);
    CHECK_NEAR(g->candidate->log_threshold, log(12.0 * 8.0 / 0.2), 1e-12);
    gateFree(g);

    /* A score gate starts its next test right away. */
    g = activeScoreGate(now);
    CHECK(ask(g, now, U_SPOT, 0.9).seq != 0);
    gateReset(g, now, EPOCH2);
    inv(g);
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    CHECK(g->serving == NULL && g->candidate != NULL && g->candidate->gen == 2);
    CHECK_EQ_INT(g->candidate->samples, 0);
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

    CHECK_EQ_INT(g->trainer.updates, 40);
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
    CHECK(g->candidate != NULL && g->candidate->gen == 3);
    CHECK_EQ_INT(g->generations, 3);
    CHECK_EQ_INT(g->candidate->samples, 0);
    CHECK_EQ_INT(g->candidate->frozen_ms, now);
    CHECK_EQ_INT(g->trainer.updates, 40);
    CHECK(mixModel(0, &g->trainer, 0) == weights);
    CHECK(mixModel(0, &g->candidate->model, 0) == weights);
    CHECK_EQ_INT(g->snapshot_updates, 40);
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
    CHECK(g->candidate != NULL && g->candidate->gen == 1);
    gateFree(g);

    g = activeScoreGate(now);
    st = g->stats;
    gateRecover(g, now, EPOCH2);
    inv(g);
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    CHECK(g->serving == NULL && g->candidate != NULL && g->candidate->gen == 2);
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
    CHECK(gateEvaluate(g, &XA, 0.0, &ev) == GATE_OK);
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
    for (i = 0; i < 28; i++) {
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
    for (i = 0; i < 28; i++) feedText(g, now, U_PLAIN, &XA, 0);
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
    gateRecover(g, T0, EPOCH2);
    gateReset(g, T0, EPOCH3);
    CHECK_EQ_INT(g->now_ms, T0 + 100 + TIMEOUT);
    CHECK_EQ_INT(g->candidate->frozen_ms, T0 + 100 + TIMEOUT);
    inv(g);
    gateFree(g);

    g = classGate(2, 12);
    train(g, T0 + 7, &XA, 0);
    CHECK_EQ_INT(g->now_ms, T0 + 7);
    train(g, T0, &XA, 0);
    CHECK_EQ_INT(g->now_ms, T0 + 7);
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

    /* A gate that should be testing a snapshot and has none (as after a
     * decode): due at any time, until maintained. */
    baseConfig(&cfg);
    g = gateAllocate(&cfg);
    if (g == NULL) die("out of memory");
    gateConfigFree(&cfg);
    CHECK(g->candidate == NULL && g->serving == NULL);
    CHECK_EQ_INT(gateMaintenanceDue(g, 0), 1);
    gateMaintain(g, 5);
    CHECK(g->candidate != NULL && g->candidate->gen == 1);
    CHECK_EQ_INT(gateMaintenanceDue(g, 5), 0);
    inv(g);
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
     * generation of 32 regions. */
    g = scoreGate();
    base = sizeof(gate) + sizeof(gateBets) + sizeof(gateDriftBets) +
           GATE_TABLE_SIZE * sizeof(gateObs *);
    CHECK_EQ_INT(expectBytes(g), base + sizeof(gateGeneration) + T * sizeof(gateRegion) +
                                     sizeof(int32_t) + sizeof(gateDrift));
    CHECK_EQ_INT(gateMemoryUsage(g), expectBytes(g));
    CHECK_EQ_INT(gateFreeEffort(g), 2);
    for (i = 0; i < 10; i++) ask(g, now, U_PLAIN, 0.9);
    CHECK_EQ_INT(gateMemoryUsage(g), expectBytes(g));
    CHECK_EQ_INT(gateMemoryUsage(g) - base - sizeof(gateGeneration) - T * sizeof(gateRegion) -
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
    for (i = 0; i < 30; i++) feedText(g, now, U_PLAIN, &XA, 0);
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
    for (i = 0; i < 28; i++) feedText(g, now, U_PLAIN, &XA, 0);
    for (i = 0; i < 50; i++) askText(g, now, U_SPOT, &XA);  /* spot checks */
    r = askText(g, now, U_PLAIN, &XB);                      /* proof, answered */
    CHECK_EQ_INT(answer(g, now, r.seq, 1), 1);
    CHECK(flagsOf(g, r.seq) & GATE_OBS_RESOLVED);
    CHECK(g->serving != NULL && g->candidate != NULL);
    CHECK_EQ_INT(g->npending, 101);
    gateFree(g);
    gateFree(NULL);
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
          g->lineage == 0 && g->now_ms == 0 && g->paused == 0 && g->runtime_tag == 0);
    CHECK(g->trainer.w != NULL && g->trainer.updates == 0 && g->table != NULL);
    CHECK(g->grid[0] == 0.5 && g->bets[1].alpha == ALPHA);
    CHECK_EQ_STR(g->cfg.labels[0], "label0");
    inv(g);

    gen = gateGenerationAllocate(g);
    if (gen == NULL) die("out of memory");
    gen->gen = 3;
    gen->log_threshold = barOf(3, 2, 4, DELTA);
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
    CHECK_EQ_INT(g->candidate->samples, 0);
    CHECK_EQ_INT(gateLearn(g, 10, 0, 5, 1), 1);
    CHECK_EQ_INT(g->candidate->samples, 2);
    CHECK_EQ_INT(g->candidate->errors, 1);
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
                CHECK(g->candidate != NULL && g->candidate->gen == 1);
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
    CHECK(g->candidate != NULL && g->candidate->gen == 1 && g->generations == 1);
    CHECK_EQ_INT(gateMaintenanceDue(g, T0), 0);
    inv(g);
    gateSparseFree(&x);
    gateFree(g);
}

/* ----------------------------- A literal model ----------------------------
 * The rules in the comment at the top of gate_core.h, written a second time
 * the slow and obvious way: one array of waiting requests in request order,
 * every question answered by a linear scan, generations held by value. It
 * shares with the gate only the betting and drift arithmetic of gate_stats
 * (tested on its own) and the threshold grid (checked in "grids" above).
 * The model does not have the classifier's weights: guesses are inputs to
 * both, and of the live model it only counts the examples. */

#define MODEL_ROWS 3

typedef struct modelGen {
    uint64_t gen; /* 0: there is no such generation */
    double bar;
    gateRegion reg[MODEL_ROWS][T];
    int32_t policy[MODEL_ROWS];
    gateDrift drift[MODEL_ROWS];
    uint64_t samples, errors, paired, audits, audit_errors, budget;
} modelGen;

typedef struct modelReq {
    uint64_t seq, cand_gen, serv_gen;
    int64_t created;
    int audit, would;   /* the draw; the serving generation would have answered */
    int cand, serv;     /* proof for generation cand_gen / serv_gen */
    int answered, missing;
    int32_t cand_label, serv_label, result;
    double cand_score, serv_score;
    uint32_t nnz;       /* clues still stored with it */
} modelReq;

typedef struct model {
    const gateConfig *cfg;
    int classifier;
    uint32_t rows;
    double grid[T];
    gateBets bets[MODEL_ROWS];
    gateDriftBets drift_bets[MODEL_ROWS];
    int64_t now;
    uint64_t epoch, generations, next_seq;
    uint64_t updates, snapshot_updates; /* examples the live model learned */
    int paused;
    modelGen cand, serv;
    modelReq *req; /* waiting requests, oldest first */
    uint32_t n;
    uint64_t nnz;  /* clues stored with them, in all */
    gateCounters st;
} model;

static int modelTested(const model *m, uint32_t row) {
    return !m->classifier || !m->cfg->class_defer[row];
}

static double modelAlpha(const model *m, uint32_t row) {
    return m->classifier ? m->cfg->class_alpha[row] : m->cfg->alpha;
}

static void modelSnapshot(model *m) {
    modelGen *c = &m->cand;
    uint32_t r, t, tested = 0;
    double need = 0.0;

    memset(c, 0, sizeof(*c));
    c->gen = ++m->generations;
    for (r = 0; r < m->rows; r++) {
        tested += (uint32_t)modelTested(m, r);
        c->policy[r] = -1;
        gateDriftInit(&c->drift[r]);
        for (t = 0; t < T; t++) gateRegionInit(&c->reg[r][t]);
    }
    c->bar = barOf(c->gen, tested, m->classifier ? 4 : T, m->cfg->delta);
    /* A classifier snapshot may use 20 times the answers a flawless region
     * of its most demanding label needs with the boldest bet. */
    for (r = 0; m->classifier && r < m->rows; r++) {
        double a = modelAlpha(m, r), n = ceil(c->bar / log(1.0 + 0.9 * a / (1.0 - a)));

        if (modelTested(m, r) && n > need) need = n;
    }
    c->budget = (uint64_t)need * 20;
    m->snapshot_updates = m->updates;
}

static int modelSnapshotDue(const model *m) {
    if (m->cand.gen) return 0;
    if (m->classifier) return m->updates - m->snapshot_updates >= m->cfg->snapshot_examples;
    return m->serv.gen == 0;
}

/* One error indicator for the guessed row of a generation. */
static void modelFeed(model *m, modelGen *gen, int32_t label, double score, int error) {
    uint32_t t, row = m->classifier ? (uint32_t)label : 0;
    int crossed[T];

    gen->samples++;
    gen->errors += (uint64_t)error;
    if (!modelTested(m, row)) return;
    for (t = 0; t < T; t++) {
        gateRegion *reg = &gen->reg[row][t];

        if (m->grid[t] <= score) {
            gateRegionUpdate(reg, &m->bets[row], error);
            if (reg->certified_at == 0 && gateRegionCertifies(reg, gen->bar))
                reg->certified_at = reg->n;
        }
        crossed[t] = reg->certified_at != 0;
    }
    /* policyByDefinition() wants the crossed regions as a mask. */
    {
        uint32_t mask = 0;

        for (t = 0; t < T; t++) mask |= (uint32_t)crossed[t] << t;
        gen->policy[row] = policyByDefinition(mask, m->classifier);
    }
}

static int modelAnswers(const model *m, const modelGen *gen) {
    uint32_t r;

    for (r = 0; r < m->rows; r++)
        if (gen->policy[r] >= 0) return 1;
    return 0;
}

/* Proof requests of a generation that its policy would answer. */
static uint64_t modelInside(const model *m, const modelGen *gen) {
    uint64_t n = 0;
    uint32_t r;

    for (r = 0; r < m->rows; r++)
        if (gen->policy[r] >= 0) n += gen->reg[r][gen->policy[r]].n;
    return n;
}

static void modelServingGone(model *m) {
    m->st.window = 0;
    m->st.coverage = 0.0;
}

static void modelApply(model *m, const modelReq *q) {
    int err_cand, err_serv;

    if (q->missing) {
        err_cand = err_serv = 1;
        m->st.missing++;
    } else if (m->classifier) {
        err_cand = q->cand_label != q->result;
        err_serv = q->serv_label != q->result;
    } else {
        err_cand = err_serv = !q->result;
    }

    if (q->cand && m->cand.gen == q->cand_gen) {
        modelGen *c = &m->cand;

        modelFeed(m, c, q->cand_label, q->cand_score, err_cand);
        if (q->would && m->serv.gen == q->serv_gen) c->paired++;
        if (modelAnswers(m, c) && (m->serv.gen == 0 || modelInside(m, c) > c->paired)) {
            m->serv = *c;
            m->serv.paired = 0;
            m->serv.budget = 0;
            c->gen = 0;
            m->st.promotions++;
            modelServingGone(m);
        } else if (c->budget && c->samples >= c->budget) {
            c->gen = 0;
            m->st.retired++;
        }
    } else if (q->cand && m->serv.gen == q->cand_gen) {
        modelFeed(m, &m->serv, q->cand_label, q->cand_score, err_cand);
    }

    if (q->serv && m->serv.gen == q->serv_gen) {
        modelGen *s = &m->serv;
        uint32_t row = m->classifier ? (uint32_t)q->serv_label : 0;

        modelFeed(m, s, q->serv_label, q->serv_score, err_serv);
        if (q->would && q->audit) {
            s->audits++;
            s->audit_errors += (uint64_t)err_serv;
            if (gateDriftPush(&s->drift[row], &m->drift_bets[row], err_serv, log(1000.0))) {
                m->serv.gen = m->cand.gen = 0;
                if (m->classifier) m->updates = 0;
                m->snapshot_updates = 0;
                m->st.revocations++;
                modelServingGone(m);
            }
        }
    }
}

static void modelRemove(model *m, uint32_t i) {
    m->nnz -= m->req[i].nnz;
    memmove(&m->req[i], &m->req[i + 1], sizeof(modelReq) * (m->n - i - 1));
    m->n--;
}

static int modelLive(const model *m, const modelReq *q) {
    if (q->cand && (m->cand.gen == q->cand_gen || m->serv.gen == q->cand_gen)) return 1;
    return q->serv && m->serv.gen == q->serv_gen;
}

/* Apply proof in request order as far as it is answered, then take a
 * snapshot if one is due. */
static void modelSettle(model *m) {
    for (;;) {
        uint32_t i = 0;
        modelReq q;

        while (i < m->n && !m->req[i].cand && !m->req[i].serv) i++;
        if (i == m->n) break;
        if (!modelLive(m, &m->req[i])) {
            /* Proof for generations that are gone. */
            if (m->req[i].answered) {
                if (m->req[i].missing) m->st.dropped++;
                modelRemove(m, i);
            } else if (m->classifier) {
                m->req[i].cand = m->req[i].serv = 0; /* its label still trains */
            } else {
                m->st.dropped++;
                modelRemove(m, i);
            }
            continue;
        }
        if (!m->req[i].answered) break;
        q = m->req[i];
        modelRemove(m, i);
        modelApply(m, &q);
    }
    if (modelSnapshotDue(m)) modelSnapshot(m);
}

static void modelMaintain(model *m, int64_t now) {
    uint32_t i = 0;

    if (now > m->now) m->now = now;
    while (i < m->n && m->req[i].created + m->cfg->feedback_timeout_ms <= m->now) {
        modelReq *q = &m->req[i];

        if (q->cand || q->serv) {
            if (!q->answered) {
                q->answered = q->missing = 1;
                m->nnz -= q->nnz;
                q->nnz = 0;
            }
            i++;
        } else {
            m->st.dropped++;
            modelRemove(m, i);
        }
    }
    modelSettle(m);
}

static void modelPredict(model *m, int64_t now, double u, const gateEval *ev, uint32_t nnz,
                         gatePredictResult *res)
{
    int audit = u < m->cfg->audit, would = 0, cand = 0, serv = 0;
    modelReq *q;

    modelMaintain(m, now);
    res->action = GATE_ACTION_ABSTAIN;
    res->label = -1;
    res->seq = 0;
    m->st.requests++;
    if (m->n >= GATE_MAX_PENDING ||
        m->n * sizeof(gateObs) + m->nnz * 8 >= GATE_MAX_PENDING_BYTES)
    {
        m->st.abstained++;
        m->st.full++;
        return;
    }
    if (m->serv.gen) {
        uint32_t row = m->classifier ? (uint32_t)ev->serving.label : 0;
        int32_t t = m->serv.policy[row];

        if (modelTested(m, row)) {
            would = t >= 0 && ev->serving.score >= m->grid[t];
            serv = t < 0 || audit;
        }
        if (m->st.window < GATE_COVERAGE_WINDOW) m->st.window++;
        m->st.coverage += ((double)would - m->st.coverage) / (double)m->st.window;
    }
    if (m->cand.gen && (m->serv.gen == 0 || audit)) cand = 1;

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
    if (!cand && !serv && !(m->classifier && m->n < GATE_TRAIN_PENDING)) return;

    q = &m->req[m->n++];
    memset(q, 0, sizeof(*q));
    q->seq = m->next_seq++;
    q->created = m->now;
    q->audit = audit;
    q->would = would;
    q->cand = cand;
    q->serv = serv;
    q->cand_label = q->serv_label = q->result = -1;
    if (m->serv.gen) {
        q->serv_gen = m->serv.gen;
        q->serv_label = ev->serving.label;
        q->serv_score = ev->serving.score;
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
    if (q->cand || q->serv) q->answered = 1; else modelRemove(m, i);
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

/* GRESET ('everything') or recovery. */
static void modelForget(model *m, int64_t now, uint64_t epoch, int everything) {
    if (now > m->now) m->now = now;
    m->n = 0;
    m->nnz = 0;
    m->cand.gen = m->serv.gen = 0;
    m->snapshot_updates = 0;
    m->epoch = epoch;
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
    for (r = 0; r < m->rows; r++) {
        gateBetsInit(&m->bets[r], modelAlpha(m, r));
        gateDriftBetsInit(&m->drift_bets[r], modelAlpha(m, r));
    }
    m->now = g->created_ms;
    m->epoch = g->epoch;
    m->next_seq = 1;
    m->req = calloc(GATE_MAX_PENDING, sizeof(modelReq));
    if (m->req == NULL) die("out of memory");
    modelSettle(m);
}

#define DIFF(same, what) do { if (!(same)) return what; } while (0)

static const char *modelDiffGen(const model *m, const modelGen *x, const gateGeneration *gen) {
    uint32_t r, t;

    DIFF((x->gen != 0) == (gen != NULL), "a generation exists in one only");
    if (gen == NULL) return NULL;
    DIFF(gen->gen == x->gen, "generation number");
    DIFF(fabs(gen->log_threshold - x->bar) <= 1e-12, "bar");
    DIFF(gen->samples == x->samples, "samples");
    DIFF(gen->errors == x->errors, "errors");
    DIFF(gen->paired == x->paired, "paired");
    DIFF(gen->audits == x->audits, "audits");
    DIFF(gen->audit_errors == x->audit_errors, "audit errors");
    DIFF(gen->budget == x->budget, "budget");
    for (r = 0; r < m->rows; r++) {
        DIFF(gen->policy[r] == x->policy[r], "policy");
        DIFF(memcmp(&gen->drift[r], &x->drift[r], sizeof(gateDrift)) == 0, "drift detector");
        for (t = 0; t < T; t++)
            DIFF(memcmp(&gen->regions[r * T + t], &x->reg[r][t], sizeof(gateRegion)) == 0,
                 "region");
    }
    return NULL;
}

/* What differs between the model and the gate, NULL if nothing does.
 * 'requests' also compares the waiting requests one by one. */
static const char *modelDiff(const model *m, const gate *g, int requests) {
    const gateCounters *a = &g->stats, *b = &m->st;
    const char *d;
    const gateObs *o = g->oldest;
    uint32_t i, evidence = 0;

    DIFF(g->now_ms == m->now, "clock");
    DIFF(g->epoch == m->epoch, "epoch");
    DIFF(g->paused == m->paused, "pause");
    DIFF(g->generations == m->generations, "ledger");
    DIFF(g->next_seq == m->next_seq, "next id");
    DIFF(!m->classifier || g->trainer.updates == m->updates, "examples learned");
    DIFF(g->snapshot_updates == m->snapshot_updates, "examples at the last snapshot");
    if ((d = modelDiffGen(m, &m->cand, g->candidate)) != NULL) return d;
    if ((d = modelDiffGen(m, &m->serv, g->serving)) != NULL) return d;
    DIFF(g->npending == m->n, "number of waiting requests");
    DIFF(g->pending_bytes == m->n * sizeof(gateObs) + m->nnz * 8, "bytes waiting");
    for (i = 0; i < m->n; i++) {
        const modelReq *q = &m->req[i];
        uint32_t flags = (q->audit ? GATE_OBS_AUDIT : 0) | (q->would ? GATE_OBS_WOULD_ACCEPT : 0) |
                         (q->cand ? GATE_OBS_CAND : 0) | (q->serv ? GATE_OBS_SERV : 0) |
                         (q->answered ? GATE_OBS_RESOLVED : 0) | (q->missing ? GATE_OBS_MISSING : 0);

        evidence += q->cand || q->serv;
        if (!requests) continue;
        DIFF(o != NULL && o->seq == q->seq, "waiting request");
        DIFF(o->created_ms == q->created, "request time");
        DIFF(o->flags == flags, "request flags");
        DIFF(o->result == q->result, "request answer");
        DIFF(o->cand_gen == q->cand_gen && o->serv_gen == q->serv_gen, "request generations");
        DIFF(o->cand_label == q->cand_label && o->serv_label == q->serv_label, "recorded labels");
        DIFF(o->cand_score == q->cand_score && o->serv_score == q->serv_score, "recorded scores");
        DIFF(o->x.n == q->nnz, "stored clues");
        o = o->next;
    }
    DIFF(g->nevidence == evidence, "proof waiting");
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
 * (compared after every operation) and a second gate that is fed only what
 * a replica receives: no ACCEPT, no request that was not recorded, a
 * maintenance call only when gateMaintenanceDue() says so, and the clock
 * the primary actually used. Whatever is replicated must end up equal. */

#define SOAK_IDS 16384 /* ids the client may still answer */
#define SOAK_GENS 1024 /* snapshots whose weights are remembered */

typedef struct soakId {
    uint64_t epoch, seq;
    int64_t asked;
    int32_t truth; /* what the teacher says */
} soakId;

typedef struct soak {
    gateConfig cfg;
    gate *g, *replica;
    model m;
    int classifier;
    uint64_t rng;
    int64_t now;
    uint32_t op;
    soakId *ids;
    uint32_t nids;
    soakId last;        /* an id that was answered already */
    uint32_t burst;     /* requests left in a burst that nobody answers */
    uint32_t broken;    /* operations left during which guesses are coin flips */
    uint32_t lossy;     /* operations left during which answers get lost */
    int huge;           /* the burst carries the largest clue vectors */
    gateSparse big;
    int failed;
    uint64_t frozen[SOAK_GENS]; /* hash of each snapshot's weights, first seen */
    /* Where the walk went. */
    uint64_t accepts, audits, promotions, revocations, retired, missing, dropped;
    uint64_t full, full_bytes, untrained, moved_down, resets, recoveries, maintained;
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

/* After every operation. */
static void soakCheck(soak *s, const char *what) {
    const gate *g = s->g;
    int walk = soakWalk(s);
    const char *d = modelDiff(&s->m, g, walk);
    char err[160] = "";
    int skip = SKIP_CLOCK | SKIP_REQUESTS | ((s->op & 255) ? SKIP_WEIGHTS : 0);
    int i;

    if (d) soakFail(s, what, d);
    if (g->npending > GATE_MAX_PENDING) soakFail(s, what, "more than 4096 waiting");
    if (g->candidate && g->serving && g->candidate->gen <= g->serving->gen)
        soakFail(s, what, "candidate older than the serving generation");
    if (gateFreeEffort(g) != 1 + g->npending + (g->candidate != NULL) + (g->serving != NULL))
        soakFail(s, what, "free effort");
    if (walk) {
        if (!gateCheckInvariants(g, err, sizeof(err))) soakFail(s, what, err);
        if (!gateCheckInvariants(s->replica, err, sizeof(err))) soakFail(s, "replica", err);
        if (gateMemoryUsage(g) != expectBytes(g)) soakFail(s, what, "memory usage");
        if (fingerprint(g, skip) != fingerprint(s->replica, skip))
            soakFail(s, what, "replica differs from primary");
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

/* Maintenance as the module runs it before looking at a request, checking
 * that gateMaintenanceDue() predicted whether anything would change. */
static void soakMaintain(soak *s, int64_t wall) {
    int64_t eff = soakEffective(s, wall);
    int due = gateMaintenanceDue(s->g, wall);
    int verify = soakWalk(s);
    uint64_t before = verify ? fingerprint(s->g, SKIP_CLOCK | SKIP_WEIGHTS) : 0;

    gateMaintain(s->g, wall);
    if (verify && due != (fingerprint(s->g, SKIP_CLOCK | SKIP_WEIGHTS) != before))
        soakFail(s, "maintain", due ? "due, but nothing changed" : "not due, but something changed");
    if (s->g->now_ms != eff) soakFail(s, "maintain", "clock");
    if (due) {
        gateMaintain(s->replica, eff);
        s->maintained++;
    }
    modelMaintain(&s->m, wall);
}

/* The gate before an operation, to see afterwards what the operation did. */
typedef struct soakBefore {
    gateCounters st;
    uint64_t serving; /* generation number, 0: none */
    int32_t policy[MODEL_ROWS];
} soakBefore;

static void soakNote(const soak *s, soakBefore *b) {
    const gate *g = s->g;
    uint32_t r;

    b->st = g->stats;
    b->serving = g->serving ? g->serving->gen : 0;
    for (r = 0; r < g->nrows; r++) b->policy[r] = g->serving ? g->serving->policy[r] : -1;
}

static void soakTally(soak *s, const soakBefore *b) {
    const gate *g = s->g;
    uint32_t r;

    if (g->stats.promotions > b->st.promotions) s->promotions++;
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

static void soakRemember(soak *s, uint64_t seq, int32_t truth) {
    soakId *id;

    if (s->lossy && soakRand(s, 10) == 0) return; /* never gets its answer */
    if (s->nids == SOAK_IDS) return;
    id = &s->ids[s->nids++];
    id->epoch = s->g->epoch;
    id->seq = seq;
    id->asked = s->now;
    id->truth = truth;
}

static void soakPredict(soak *s, double u) {
    gate *g = s->g;
    int64_t wall = soakWall(s), eff;
    soakBefore before;
    gatePredictResult res, expect, rres;
    gateSparse x = {0, NULL, NULL}, mine = {0, NULL, NULL};
    gateEval ev;
    double score = 0.0;
    int32_t truth;
    int needs, was_full;

    soakMaintain(s, wall);
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
    if (gateEvaluate(g, s->classifier ? &x : NULL, score, &ev) != GATE_OK)
        soakFail(s, "predict", "gateEvaluate failed");
    if (s->classifier) {
        /* The guesses are those of the frozen models. */
        double sc;

        if (ev.has_serving != (g->serving != NULL) || ev.has_candidate != (g->candidate != NULL))
            soakFail(s, "predict", "guesses for generations that are not there");
        if (g->serving && (gateModelPredict(&g->serving->model, &x, NULL, &sc) != ev.serving.label ||
                           sc != ev.serving.score))
            soakFail(s, "predict", "serving guess");
        if (g->candidate && (gateModelPredict(&g->candidate->model, &x, NULL, &sc) !=
                             ev.candidate.label || sc != ev.candidate.score))
            soakFail(s, "predict", "candidate guess");
        if (gateSparseCopy(&mine, &x) != GATE_OK) die("out of memory");
    }
    needs = gatePredictNeedsFeatures(g, u, &ev);
    was_full = gateFull(g);
    if (gatePredict(g, wall, u, &ev, s->classifier ? &mine : NULL, &res) != GATE_OK)
        soakFail(s, "predict", "gatePredict failed");
    modelPredict(&s->m, wall, u, &ev, x.n, &expect);

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
    if (s->classifier) {
        /* Recorded: the gate took the clues. Otherwise they are untouched. */
        if (res.seq ? (mine.n != 0 || mine.idx != NULL || mine.val != NULL)
                    : (mine.n != x.n || (x.n && mine.idx == NULL)))
            soakFail(s, "predict", "ownership of the clues");
        gateSparseFree(&mine);
    }

    if (res.seq != 0) {
        /* What a replica is sent: the realized clock, draw and guesses,
         * and the clues only because the request was recorded. */
        if (s->classifier && gateSparseCopy(&mine, &x) != GATE_OK) die("out of memory");
        if (gatePredict(s->replica, eff, u, &ev, s->classifier ? &mine : NULL, &rres) != GATE_OK ||
            rres.seq != res.seq || rres.action != res.action)
            soakFail(s, "predict", "replica replied differently");
        gateSparseFree(&mine);
        soakRemember(s, res.seq, truth);
    }
    gateSparseFree(&x);

    if (res.action == GATE_ACTION_ACCEPT) s->accepts++;
    if (res.action == GATE_ACTION_AUDIT) s->audits++;
    if (was_full) {
        s->full++;
        if (g->npending < GATE_MAX_PENDING) s->full_bytes++;
    } else if (s->classifier && res.seq == 0 && res.action == GATE_ACTION_ABSTAIN) {
        s->untrained++;
    }
    soakTally(s, &before);
    soakCheck(s, "predict");
}

/* GLEARN with whatever id, on all three. */
static void soakLearn(soak *s, const soakId *id, const char *what) {
    int64_t wall = soakWall(s), eff = soakEffective(s, wall);
    soakBefore before;
    int used;

    soakNote(s, &before);
    used = gateLearn(s->g, wall, id->epoch, id->seq, id->truth);
    if (used != modelLearn(&s->m, wall, id->epoch, id->seq, id->truth))
        soakFail(s, what, "gateLearn differs from the model");
    if (gateLearn(s->replica, eff, id->epoch, id->seq, id->truth) != used)
        soakFail(s, what, "replica used the answer differently");
    soakTally(s, &before);
    soakCheck(s, what);
}

static void soakAnswer(soak *s) {
    uint32_t i = 0;

    /* The client gives up on what it could not answer in two windows. */
    while (i < s->nids && s->ids[i].asked + 2 * TIMEOUT < s->now) i++;
    memmove(s->ids, s->ids + i, sizeof(soakId) * (s->nids - i));
    s->nids -= i;
    if (s->nids == 0) return;
    /* Mostly the oldest ids, sometimes any: answers overtake each other,
     * and some come when the window has closed. */
    i = soakRand(s, 10) < 8 ? soakRand(s, s->nids < 4 ? s->nids : 4) : soakRand(s, s->nids);
    s->last = s->ids[i];
    memmove(&s->ids[i], &s->ids[i + 1], sizeof(soakId) * (s->nids - i - 1));
    s->nids--;
    soakLearn(s, &s->last, "answer");
}

/* Answers nobody asked for. */
static void soakStrayAnswer(soak *s) {
    soakId id;

    id.epoch = s->g->epoch;
    id.truth = (int32_t)soakRand(s, s->classifier ? 3 : 2);
    switch (soakRand(s, 5)) {
    case 0: id.seq = 0; break;
    case 1: id.seq = s->g->next_seq + soakRand(s, 3); break;
    case 2: /* a waiting request under another gate's epoch */
        id.seq = s->g->oldest ? s->g->oldest->seq : 1;
        id.epoch ^= 1;
        break;
    case 3: id = s->last; break; /* an answer given twice */
    default: id.seq = 1 + soakRand(s, (uint32_t)s->g->next_seq); break;
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
        gateReset(s->g, wall, epoch);
        gateReset(s->replica, eff, epoch);
        modelForget(&s->m, wall, epoch, 1);
        s->resets++;
        break;
    default:
        gateRecover(s->g, wall, epoch);
        gateRecover(s->replica, eff, epoch);
        modelForget(&s->m, wall, epoch, 0);
        s->recoveries++;
        break;
    }
    soakCheck(s, "administration");
}

static void soakStep(soak *s) {
    uint32_t dice = soakRand(s, 100000);
    double u = gate_test_uniform(&s->rng);

    switch (soakRand(s, 8)) {
    case 0: u = 0.0; break;
    case 1: u = s->cfg.audit; break; /* the largest draw that is no spot check */
    default: break;
    }
    if (s->broken) s->broken--;
    if (s->lossy) s->lossy--;
    if (s->burst) {
        /* Nobody answers and no time passes: the gate fills up. */
        s->burst--;
        soakPredict(s, U_SPOT);
        return;
    }
    s->huge = 0;
    s->now += soakRand(s, 3);
    if (dice < 40000) soakPredict(s, u);
    else if (dice < 85000) soakAnswer(s);
    else if (dice < 88000) soakStrayAnswer(s);
    else if (dice < 93000 && s->classifier) soakTrain(s);
    else if (dice < 98000) soakMaintain(s, soakWall(s)), soakCheck(s, "maintain");
    else if (dice < 98100) soakAdmin(s, 1);
    else if (dice < 98500) soakAdmin(s, 0);
    else if (dice < 98530) soakAdmin(s, 2);
    else if (dice < 98560) soakAdmin(s, 3);
    else if (dice < 98610) {
        s->now += TIMEOUT + soakRand(s, 50); /* everything waiting times out */
        soakMaintain(s, s->now);
        soakCheck(s, "timeout");
    } else if (dice < 98630) {
        s->broken = 800;
    } else if (dice < 98660) {
        s->lossy = 500;
    } else if (dice < 98666) {
        s->burst = GATE_MAX_PENDING + 200;
    } else if (dice < 98672 && s->classifier) {
        s->burst = 300;
        s->huge = 1;
    } else {
        soakPredict(s, u);
    }
}

static void soakRun(soak *s, int classifier, uint64_t seed, uint32_t ops) {
    char err[128];
    uint32_t i;

    memset(s, 0, sizeof(*s));
    s->classifier = classifier;
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
    }
    CHECK(!s->failed);

    gateFree(s->g);
    gateFree(s->replica);
    gateConfigFree(&s->cfg);
    free(s->m.req);
    free(s->ids);
    free(s->big.idx);
    free(s->big.val);
}

/* Where the walks of several seeds went, added up. */
static void soakAdd(soak *sum, const soak *s) {
    sum->accepts += s->accepts;
    sum->audits += s->audits;
    sum->promotions += s->promotions;
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
}

TEST(soak_score) {
    static const uint64_t seeds[] = {5, 8};
    soak s, sum;
    size_t i;

    memset(&sum, 0, sizeof(sum));
    for (i = 0; i < COUNTOF(seeds); i++) {
        soakRun(&s, 0, seeds[i], 60000);
        soakAdd(&sum, &s);
    }
    /* The walks must have been through everything they are there for. */
    CHECK(sum.accepts >= 1000 && sum.audits >= 1000);
    CHECK(sum.promotions >= 10);
    CHECK(sum.moved_down >= 10); /* thresholds proven after promotion */
    CHECK(sum.revocations >= 3);
    CHECK(sum.missing >= 50 && sum.dropped >= 1);
    CHECK(sum.full >= 100);
    CHECK(sum.resets >= 5 && sum.recoveries >= 5);
    CHECK(sum.maintained >= 50); /* maintenance a replica had to be told of */
}

TEST(soak_classifier) {
    static const uint64_t seeds[] = {4, 16};
    soak s, sum;
    size_t i;

    memset(&sum, 0, sizeof(sum));
    for (i = 0; i < COUNTOF(seeds); i++) {
        soakRun(&s, 1, seeds[i], 30000);
        soakAdd(&sum, &s);
    }
    CHECK(sum.accepts >= 300 && sum.audits >= 100);
    CHECK(sum.promotions >= 5);
    CHECK(sum.moved_down >= 5);
    CHECK(sum.retired >= 1);     /* a snapshot dropped at its budget */
    CHECK(sum.revocations >= 1);
    CHECK(sum.missing >= 50 && sum.dropped >= 20);
    CHECK(sum.full >= 100 && sum.full_bytes >= 10);
    CHECK(sum.untrained >= 10);  /* the limit on requests kept for training */
    CHECK(sum.resets >= 3 && sum.recoveries >= 3);
}

int main(void) {
    gateSetAllocator(testMalloc, testCalloc, testRealloc, testFree);

    RUN_LEAKFREE(expected_numbers);
    RUN_LEAKFREE(names);
    RUN_LEAKFREE(config_defaults);
    RUN_LEAKFREE(config_classes);
    RUN_LEAKFREE(config_validate);
    RUN_LEAKFREE(config_copy_and_free);
    RUN_LEAKFREE(grids);
    RUN_LEAKFREE(score_life);
    RUN_LEAKFREE(continued_testing);
    RUN_LEAKFREE(coverage_average);
    RUN_LEAKFREE(evaluate);
    RUN_LEAKFREE(classifier_life);
    RUN_LEAKFREE(frozen_guesses);
    RUN_LEAKFREE(asked_before_snapshot);
    RUN_LEAKFREE(proof_survives_promotion);
    RUN_LEAKFREE(answers_wait_for_their_turn);
    RUN_LEAKFREE(answer_order_does_not_matter);
    RUN_LEAKFREE(answer_order_does_not_matter_classifier);
    RUN_LEAKFREE(feedback_ids);
    RUN_LEAKFREE(missing_answers);
    RUN_LEAKFREE(missing_is_a_wrong_answer);
    RUN_LEAKFREE(entry_points);
    RUN_LEAKFREE(crossed_is_not_proven);
    RUN_LEAKFREE(bar_per_generation);
    RUN_LEAKFREE(replacing_needs_more_accepts);
    RUN_LEAKFREE(candidate_budget);
    RUN_LEAKFREE(score_candidate_has_no_budget);
    RUN_LEAKFREE(drift_score);
    RUN_LEAKFREE(drift_classifier);
    RUN_LEAKFREE(full_gate);
    RUN_LEAKFREE(training_only_limit);
    RUN_LEAKFREE(full_by_bytes);
    RUN_LEAKFREE(pause);
    RUN_LEAKFREE(reset);
    RUN_LEAKFREE(recover);
    RUN_LEAKFREE(clues_needed_and_owned);
    RUN_LEAKFREE(predict_refuses_bad_input);
    RUN_LEAKFREE(clock);
    RUN_LEAKFREE(maintenance_due);
    RUN_LEAKFREE(memory_usage);
    RUN_LEAKFREE(free_with_waiting_requests);
    RUN_LEAKFREE(constructors);
    RUN_LEAKFREE(allocation_failures);
    RUN_LEAKFREE(soak_score);
    RUN_LEAKFREE(soak_classifier);
    return TEST_REPORT();
}
