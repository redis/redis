/*
 * Gate data type: unit tests of the gate state machine (gate_core.c).
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Every test drives a gate with explicit clocks and audit bits, and checks
 * gateCheckInvariants() after every step. Expected values are derived here,
 * from the protocol, never read back from the implementation:
 *
 * THE NUMBERS. Most gates below use ALPHA 0.2, DELTA 0.1 and 4 thresholds.
 * Generation g gets delta_g = 0.1 / (g (g+1)) and each of its regions
 * delta_g / regions, so a pooled region of generation 1 must reach a log
 * wealth of -ln(0.05 / 4) = ln 80 = 4.3820. A correct sample multiplies the
 * wealth of betting fraction c by 1 + c * alpha / (1 - alpha) = 1 + c / 4
 * and an error multiplies it by 1 - c, c in {0.1, 0.25, 0.5, 0.9}. After n
 * correct samples the mixture is (1.025^n + 1.0625^n + 1.125^n + 1.225^n)/4,
 * which is 67.7 (ln 4.215) at n = 27 and 82.0 (ln 4.407) at n = 28: the
 * first generation certifies on its 28th clean sample, not before. The same
 * arithmetic (steps_to_certify() below, cross-checked against constants
 * computed with Python) gives every other promotion step asserted here:
 *
 *     generation 1, pooled, no error              28
 *     generation 1, pooled, error first           40
 *     generation 1, pooled, three errors first    63
 *     generation 1, pooled, errors at 5 and 30    52
 *     generation 2, pooled, no error              34   (ln 240)
 *     generation 2, pooled, error first           46
 *     generation 1, two class rows, alpha 0.2     32   (ln 160)
 *     generation 1, two class rows, alpha 0.1     68
 */

#include "testutil.h"
#include "gate_core.h"
#include "gate_internal.h"

#define T0 1700000000000ll        /* a wall clock in unix milliseconds */
#define LINEAGE 0x0123456789abcdefull
#define EPOCH 0xfeedfacecafebeefull
#define TTL 1000                  /* OBSERVATIONTTL of the test gates */
#define CERT_TTL 100000000ll      /* CERTTTL of the test gates */

/* ------------------------- Counting allocator ----------------------------
 * Installed for the whole run: every block the library allocates is counted,
 * so a leak fails the run even without valgrind, and any single allocation
 * can be made to fail. */

static long live_blocks = 0; /* allocated through the hooks, not freed yet */
static long alloc_calls = 0; /* allocation attempts since the last reset */
static long fail_at = 0;     /* ordinal of the attempt to fail, 0 = none */

static int alloc_must_fail(void) { return ++alloc_calls == fail_at; }

static void *t_malloc(size_t n) {
    void *p;
    if (alloc_must_fail()) return NULL;
    p = malloc(n);
    if (p != NULL) live_blocks++;
    return p;
}

static void *t_calloc(size_t nmemb, size_t size) {
    void *p;
    if (alloc_must_fail()) return NULL;
    p = calloc(nmemb, size);
    if (p != NULL) live_blocks++;
    return p;
}

static void *t_realloc(void *old, size_t n) {
    void *p;
    if (alloc_must_fail()) return NULL;
    p = realloc(old, n);
    if (p != NULL && old == NULL) live_blocks++;
    return p;
}

static void t_free(void *p) {
    live_blocks--;
    free(p);
}

/* ------------------------- Independent arithmetic ------------------------ */

/* Number of samples after which one region certifies, for a stream that is
 * correct except at the listed 1-based positions. Straight from the
 * definition of the mixture e-process; shares no code with gate_stats.c. */
static int steps_to_certify(double alpha, double delta_region, const int *err_at, int nerr) {
    static const double c[4] = {0.1, 0.25, 0.5, 0.9};
    double lw[4] = {0, 0, 0, 0}, threshold = -log(delta_region);
    int n, j, k;

    for (n = 1; n <= 100000; n++) {
        double max, sum = 0;
        int error = 0;

        for (k = 0; k < nerr; k++)
            if (err_at[k] == n) error = 1;
        for (j = 0; j < 4; j++)
            lw[j] += error ? log1p(-c[j]) : log1p(c[j] * alpha / (1.0 - alpha));
        if (error) continue; /* an error only lowers the wealth */
        max = lw[0];
        for (j = 1; j < 4; j++)
            if (lw[j] > max) max = lw[j];
        for (j = 0; j < 4; j++) sum += exp(lw[j] - max);
        if (max + log(sum / 4.0) >= threshold) return n;
    }
    return -1;
}

/* ------------------------------ Check helpers ---------------------------- */

#define CHECK_STATE(g, st, why) do { \
    CHECK_EQ_STR(gateStateName(gateState(g)), gateStateName(st)); \
    CHECK_EQ_STR(gateReasonName(gateIdleReason(g)), gateReasonName(why)); \
} while (0)

/* 'r' is evaluated once: it is often a call that issues a request. */
#define CHECK_RES(r, act, why) do { \
    gatePredictResult _res = (r); \
    CHECK_EQ_STR(gateActionName(_res.action), gateActionName(act)); \
    CHECK_EQ_STR(gateReasonName(_res.reason), gateReasonName(why)); \
} while (0)

#define CHECK_FB(status, expected) \
    CHECK_EQ_STR(gateFeedbackStatusName(status), gateFeedbackStatusName(expected))

/* Called after every step of every test. */
static void check_gate(const gate *g, int line) {
    char err[256];
    gateStateId st;
    gateReason why;

    gate_test_checks++;
    if (!gateCheckInvariants(g, err, sizeof(err))) {
        gate_test_failures++;
        printf("  %s:%d: in %s: invariant violated: %s\n", __FILE__, line, gate_test_current, err);
    }
    /* At the gate's own clock a preview has nothing left to anticipate, and
     * an older clock is ignored. */
    gate_test_checks++;
    gatePreview(g, g->now_ms, 0, &st, &why);
    if (st != gateState(g) || why != gateIdleReason(g)) {
        gate_test_failures++;
        printf("  %s:%d: in %s: preview %s/%s differs from state %s/%s\n", __FILE__, line,
               gate_test_current, gateStateName(st), gateReasonName(why),
               gateStateName(gateState(g)), gateReasonName(gateIdleReason(g)));
    }
    gate_test_checks++;
    gatePreview(g, g->now_ms - 12345, 0, &st, &why);
    if (st != gateState(g) || why != gateIdleReason(g)) {
        gate_test_failures++;
        printf("  %s:%d: in %s: preview with an old clock differs\n", __FILE__, line,
               gate_test_current);
    }
}
#define INV(g) check_gate(g, __LINE__)

/* ------------------------------ Fingerprint ------------------------------
 * A hash of the complete logical state of a gate. Two gates that went
 * through equivalent histories must have equal fingerprints, and a call
 * documented to change nothing must leave the fingerprint alone. */

#define FP_ALL 0u
#define FP_NO_FEEDBACK (1u << 0) /* leave out stats.feedback[] */

static uint64_t fp_u64(uint64_t h, uint64_t v) {
    h = (h ^ v) * 0x100000001b3ull;
    return h ^ (h >> 31);
}
static uint64_t fp_dbl(uint64_t h, double d) {
    uint64_t u;
    memcpy(&u, &d, sizeof(u));
    return fp_u64(h, u);
}
static uint64_t fp_str(uint64_t h, const char *s) {
    for (; *s; s++) h = fp_u64(h, (unsigned char)*s);
    return fp_u64(h, 0x100);
}
static uint64_t fp_model(uint64_t h, const gateModel *m) {
    uint32_t i;

    h = fp_u64(h, m->dim);
    h = fp_u64(h, m->nclasses);
    h = fp_u64(h, m->updates);
    if (m->w == NULL) return fp_u64(h, 0xdead);
    for (i = 0; i < m->nvec * m->dim; i++) h = fp_dbl(h, m->w[i]);
    for (i = 0; i < m->nvec; i++) h = fp_dbl(h, m->b[i]);
    return h;
}
static uint64_t fp_generation(uint64_t h, const gate *g, const gateGeneration *gen) {
    uint32_t i, j;

    if (gen == NULL) return fp_u64(h, 0xabcdef);
    h = fp_u64(h, gen->gen);
    h = fp_u64(h, (uint64_t)gen->frozen_ms);
    h = fp_u64(h, (uint64_t)gen->promoted_ms);
    h = fp_u64(h, (uint64_t)gen->expires_ms);
    h = fp_dbl(h, gen->delta_gen);
    h = fp_dbl(h, gen->delta_region);
    h = fp_dbl(h, gen->log_threshold);
    h = fp_u64(h, gen->regions_tested);
    h = fp_u64(h, gen->samples);
    h = fp_u64(h, gen->errors);
    h = fp_u64(h, gen->imputed);
    h = fp_u64(h, gen->policy_updates);
    h = fp_u64(h, (uint64_t)gen->closed);
    h = fp_model(h, &gen->model);
    for (i = 0; i < g->nrows * g->cfg.nthresholds; i++) {
        const gateRegion *r = &gen->regions[i];
        for (j = 0; j < GATE_NBETS; j++) h = fp_dbl(h, r->logw[j]);
        h = fp_u64(h, r->n);
        h = fp_u64(h, r->errors);
        h = fp_u64(h, r->certified_at);
    }
    for (i = 0; i < g->nrows; i++) {
        const gateDriftWindow *w = &gen->drift[i];
        h = fp_u64(h, (uint64_t)(int64_t)gen->policy[i]);
        for (j = 0; j < GATE_DRIFT_WINDOW / 64; j++) h = fp_u64(h, w->bits[j]);
        h = fp_u64(h, w->n);
        h = fp_u64(h, w->pos);
        h = fp_u64(h, w->errors);
        h = fp_u64(h, w->total);
        h = fp_u64(h, w->total_errors);
    }
    return h;
}
static uint64_t fingerprint(const gate *g, unsigned what) {
    uint64_t h = 0xcbf29ce484222325ull;
    const gateCounters *s = &g->stats;
    const gateObs *o;
    uint32_t i;

    h = fp_u64(h, g->lineage);
    h = fp_u64(h, g->epoch);
    h = fp_u64(h, (uint64_t)g->created_ms);
    h = fp_u64(h, (uint64_t)g->now_ms);
    h = fp_u64(h, g->generations);
    h = fp_u64(h, g->recoveries);
    h = fp_u64(h, (uint64_t)g->paused);
    h = fp_u64(h, (uint64_t)g->recovery_required);
    h = fp_u64(h, (uint64_t)g->revoked_reason);
    h = fp_u64(h, (uint64_t)g->last_revoked_reason);
    h = fp_u64(h, (uint64_t)g->last_revoked_ms);
    h = fp_u64(h, g->last_revoked_gen);
    h = fp_u64(h, g->next_seq);
    h = fp_u64(h, g->expired_through);
    h = fp_u64(h, g->invalid_below);
    h = fp_u64(h, g->npending);
    h = fp_u64(h, g->neligible);
    h = fp_u64(h, g->pending_bytes);
    h = fp_u64(h, g->fixed_bytes);
    h = fp_u64(h, g->runtime_tag);
    h = fp_dbl(h, g->cfg.audit);
    h = fp_dbl(h, g->cfg.cold_audit);
    h = fp_str(h, g->cfg.teacher);
    h = fp_str(h, g->cfg.schema_version);
    h = fp_model(h, &g->trainer);
    h = fp_generation(h, g, g->candidate);
    h = fp_generation(h, g, g->serving);

    h = fp_u64(h, s->requests);
    h = fp_u64(h, s->accepted);
    h = fp_u64(h, s->audited);
    h = fp_u64(h, s->abstained);
    h = fp_u64(h, s->would_accept);
    for (i = 0; i < GATE_REASON_COUNT; i++) h = fp_u64(h, s->abstain_reason[i]);
    h = fp_u64(h, s->observations);
    h = fp_u64(h, s->eligible);
    if (!(what & FP_NO_FEEDBACK))
        for (i = 0; i < GATE_FB_STATUS_COUNT; i++) h = fp_u64(h, s->feedback[i]);
    h = fp_u64(h, s->expired);
    h = fp_u64(h, s->expired_eligible);
    h = fp_u64(h, s->resource_limited);
    h = fp_u64(h, s->imputed);
    h = fp_u64(h, s->train_rejected);
    h = fp_u64(h, s->promotions);
    h = fp_u64(h, s->revocations);
    h = fp_u64(h, s->candidates_discarded);

    for (o = g->oldest; o != NULL; o = o->next) {
        h = fp_u64(h, o->seq);
        h = fp_u64(h, (uint64_t)o->created_ms);
        h = fp_u64(h, o->flags);
        h = fp_u64(h, (uint64_t)(int64_t)o->cand_label);
        h = fp_u64(h, (uint64_t)(int64_t)o->serv_label);
        h = fp_dbl(h, o->cand_score);
        h = fp_dbl(h, o->serv_score);
        h = fp_u64(h, o->cand_gen);
        h = fp_u64(h, o->serv_gen);
        h = fp_u64(h, (uint64_t)(int64_t)o->result);
        h = fp_u64(h, o->x.n);
        for (i = 0; i < o->x.n; i++) {
            h = fp_u64(h, o->x.idx[i]);
            h = fp_dbl(h, o->x.val[i]);
        }
    }
    for (o = g->ehead; o != NULL; o = o->enext) h = fp_u64(h, o->seq);
    return h;
}

/* ----------------------------- Configurations ---------------------------- */

static char *dupstr(const char *s) {
    size_t n = strlen(s) + 1;
    char *copy = gate_malloc(n);

    memcpy(copy, s, n);
    return copy;
}

/* SCORE gate, one pooled row, thresholds {0, 1/3, 2/3, 1}. */
static void cfg_score(gateConfig *c) {
    gateConfigInit(c);
    c->mode = GATE_MODE_SCORE;
    c->alpha = 0.2;
    c->delta = 0.1;
    c->nthresholds = 4;
    c->audit = 0.25;
    c->cold_audit = 0.5;
    c->observation_ttl_ms = TTL;
    c->cert_ttl_ms = CERT_TTL;
    c->teacher = dupstr("teacher-v1");
    c->schema_version = dupstr("schema-v1");
}

static void cfg_labels(gateConfig *c, uint32_t k, const char *const *names) {
    uint32_t i;

    c->nclasses = k;
    c->labels = gate_calloc(k, sizeof(char *));
    for (i = 0; i < k; i++) c->labels[i] = dupstr(names[i]);
}

/* Class-conditional SCORE gate: a (alpha 0.2), b (alpha 0.1), c ALWAYSDEFER. */
static void cfg_classes(gateConfig *c) {
    static const char *const names[3] = {"a", "b", "c"};

    cfg_score(c);
    cfg_labels(c, 3, names);
    c->class_mode = 1;
    c->class_alpha = gate_malloc(3 * sizeof(double));
    c->class_alpha[0] = 0.2;
    c->class_alpha[1] = 0.1;
    c->class_alpha[2] = 0.2;
    c->class_defer = gate_malloc(3);
    c->class_defer[0] = 0;
    c->class_defer[1] = 0;
    c->class_defer[2] = 1;
}

/* CLASSIFIER gate: classes no / yes, 16 named-feature buckets, one pooled
 * row, thresholds {0.5, 0.9556, 0.99785, 0.9999} (uniform in log-odds). */
static void cfg_classifier(gateConfig *c) {
    static const char *const names[2] = {"no", "yes"};

    cfg_score(c);
    c->mode = GATE_MODE_CLASSIFIER;
    cfg_labels(c, 2, names);
    c->schema.inputs = GATE_INPUT_FEATURES;
    c->schema.feature_dim = 16;
    c->lr = 1.0;
}

/* Validate, create, release the config. */
static gate *mk(gateConfig *c) {
    char err[256] = "";
    gate *g;

    if (gateConfigValidate(c, err, sizeof(err)) != GATE_OK) {
        printf("test configuration does not validate: %s\n", err);
        exit(1);
    }
    g = gateCreate(c, LINEAGE, EPOCH, T0);
    gateConfigFree(c);
    if (g == NULL) {
        printf("gateCreate failed\n");
        exit(1);
    }
    INV(g);
    return g;
}

/* ------------------------------ SCORE drivers ---------------------------- */

/* One GPREDICT ... SCORE request, the way the module issues it. */
static gatePredictResult req(gate *g, int64_t now, int audit, double score, int32_t label) {
    gatePredictResult res;
    gateEval ev;

    memset(&res, 0x5a, sizeof(res));
    gateMaintain(g, now);
    CHECK(gateEvaluate(g, NULL, score, label, &ev) == GATE_OK);
    CHECK(gatePredictNeedsFeatures(g, audit, &ev) == 0);
    CHECK(gatePredict(g, now, audit, &ev, NULL, &res) == GATE_OK);
    INV(g);
    return res;
}

/* One GLEARN ... CORRECT. */
static gateFeedbackStatus fb(gate *g, int64_t now, uint64_t seq, int correct) {
    gateFeedback f = {GATE_FEEDBACK_CORRECT, -1, NULL, correct};
    gateFeedbackStatus st = gateLearn(g, now, g->epoch, seq, &f, NULL);

    INV(g);
    return st;
}

/* An audited request answered at once: one eligible sample. */
static uint64_t sample(gate *g, int64_t now, double score, int32_t label, int correct) {
    gatePredictResult r = req(g, now, 1, score, label);

    CHECK(r.seq != 0);
    if (r.seq != 0) CHECK_FB(fb(g, now, r.seq, correct), GATE_FB_APPLIED);
    return r.seq;
}

/* Freeze a generation and certify its region "score >= 2/3" only: three
 * errors at 0.5 fall in regions 0 and 1 and hold them back, 28 clean
 * samples at 0.9 certify region 2 (they are in 0 and 1 too, which then have
 * 28 correct and 3 errors: far from certified). 31 requests in all. */
static int64_t promote_at_two_thirds(gate *g, int64_t now) {
    uint64_t gen = 0;
    int i;

    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    for (i = 0; i < 3; i++) sample(g, ++now, 0.5, -1, 0);
    for (i = 0; i < 28; i++) sample(g, ++now, 0.9, -1, 1);
    CHECK(g->serving != NULL && g->serving->gen == gen && g->serving->policy[0] == 2);
    return now;
}

/* Freeze a generation and certify everything: clean samples at 1.0 are in
 * all four regions, so the policy is threshold 0 and every request is a
 * would-accept. Generation g of a pooled gate needs steps_to_certify() of
 * them for its share 0.1 / (g (g+1)) / 4 (28 for the first), and is checked
 * to need exactly that many. */
static int64_t promote_everything(gate *g, int64_t now) {
    uint64_t gen = 0;
    int i, need;

    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    need = steps_to_certify(0.2, 0.1 / ((double)gen * ((double)gen + 1.0)) / 4, NULL, 0);
    for (i = 1; i < need; i++) sample(g, ++now, 1.0, -1, 1);
    CHECK(g->candidate != NULL && g->candidate->gen == gen); /* not one sample early */
    sample(g, ++now, 1.0, -1, 1);
    CHECK(g->serving != NULL && g->serving->gen == gen && g->serving->policy[0] == 0);
    return now;
}

/* ---------------------------- CLASSIFIER drivers ------------------------- */

typedef struct vec {
    uint32_t n;
    uint32_t idx[16];
    float val[16];
} vec;

static const vec VA = {1, {0}, {1.0f}}; /* the teacher says "yes" */
static const vec VB = {1, {1}, {1.0f}}; /* the teacher says "no" */
static const vec VC = {1, {5}, {1.0f}}; /* a feature the model never saw */
static const vec V0 = {0, {0}, {0.0f}}; /* a present, empty request */

static gateSparse vec_sparse(const vec *v) {
    gateSparse x = {0, NULL, NULL};

    if (v->n != 0) {
        x.idx = gate_malloc(v->n * sizeof(uint32_t));
        x.val = gate_malloc(v->n * sizeof(float));
        memcpy(x.idx, v->idx, v->n * sizeof(uint32_t));
        memcpy(x.val, v->val, v->n * sizeof(float));
        x.n = v->n;
    }
    return x;
}

/* Prediction of one model for a test vector. */
static int model_says(const gateModel *m, const vec *v, double *score) {
    gateSparse x = vec_sparse(v);
    int label = gateModelPredict(m, &x, NULL, score);

    gateSparseFree(&x);
    return label;
}

/* One GPREDICT with features. Checks, on every call, that the ownership
 * rule for the feature vector holds and that gatePredictNeedsFeatures()
 * told the truth. */
static gatePredictResult creq(gate *g, int64_t now, int audit, const vec *v) {
    gateSparse x = vec_sparse(v), given;
    gatePredictResult res;
    gateEval ev;
    int needs;

    memset(&res, 0x5a, sizeof(res));
    gateMaintain(g, now);
    CHECK(gateEvaluate(g, &x, 0.0, -1, &ev) == GATE_OK);
    needs = gatePredictNeedsFeatures(g, audit, &ev);
    given = x;
    CHECK(gatePredict(g, now, audit, &ev, &x, &res) == GATE_OK);
    if (res.seq != 0) {
        const gateObs *o = gateFindObservation(g, res.seq);

        CHECK(needs == 1);
        CHECK(x.n == 0 && x.idx == NULL && x.val == NULL); /* the gate owns it now */
        CHECK(o != NULL && o->x.n == given.n && o->x.idx == given.idx && o->x.val == given.val);
    } else {
        CHECK(x.n == given.n && x.idx == given.idx && x.val == given.val); /* untouched */
        CHECK(needs == (res.action != GATE_ACTION_ACCEPT &&
                        res.reason != GATE_REASON_RECOVERY_REQUIRED));
        gateSparseFree(&x);
    }
    INV(g);
    return res;
}

/* One GLEARN ... LABEL. */
static gateFeedbackStatus clearn(gate *g, int64_t now, uint64_t seq, int32_t label) {
    gateFeedback f = {GATE_FEEDBACK_LABEL, label, NULL, 0};
    gateFeedbackStatus st = gateLearn(g, now, g->epoch, seq, &f, NULL);

    INV(g);
    return st;
}

/* One GADD ... LABEL on a two class gate. */
static int ctrain(gate *g, int64_t now, const vec *v, int32_t label) {
    double q[2] = {label == 0, label == 1};
    gateSparse x = vec_sparse(v);
    int rc = gateTrain(g, now, &x, q);

    gateSparseFree(&x);
    INV(g);
    return rc;
}

/* GADD until the trainer is sure that A is "yes" and B is "no". C stays a
 * toss-up: its only feature was never trained, so its logit is the bias. */
static void train_ab(gate *g, int64_t now) {
    double sa = 0, sb = 0, sc = 0;
    int i;

    for (i = 0; i < 400; i++) {
        CHECK(ctrain(g, now, &VA, 1) == GATE_OK);
        CHECK(ctrain(g, now, &VB, 0) == GATE_OK);
    }
    CHECK(model_says(&g->trainer, &VA, &sa) == 1);
    CHECK(model_says(&g->trainer, &VB, &sb) == 0);
    model_says(&g->trainer, &VC, &sc);
    /* A and B land in the region "score >= 0.9556" (index 1) or above, C
     * only in the accept-everything region (index 0). */
    CHECK(gateGridCount(g->grid, 4, sa) >= 2);
    CHECK(gateGridCount(g->grid, 4, sb) >= 2);
    CHECK_EQ_INT(gateGridCount(g->grid, 4, sc), 1);
}

/* An audited classifier request answered at once. */
static uint64_t csample(gate *g, int64_t now, const vec *v, int32_t teacher_label) {
    gatePredictResult r = creq(g, now, 1, v);

    CHECK(r.seq != 0);
    if (r.seq != 0) CHECK_FB(clearn(g, now, r.seq, teacher_label), GATE_FB_APPLIED);
    return r.seq;
}

/* Train, freeze and certify the region "score >= 0.9556" only: three
 * low-confidence mistakes on C hold region 0 back, 28 confident, correct
 * predictions on A certify region 1. */
static int64_t classifier_promote(gate *g, int64_t now) {
    uint64_t gen = 0;
    double sc;
    int i, lc;

    train_ab(g, now);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    lc = model_says(&g->candidate->model, &VC, &sc);
    for (i = 0; i < 3; i++) csample(g, ++now, &VC, 1 - lc); /* the teacher disagrees */
    for (i = 0; i < 28; i++) csample(g, ++now, &VA, 1);
    CHECK(g->serving != NULL && g->serving->gen == gen && g->serving->policy[0] == 1);
    return now;
}

/* ================================= Tests ================================= */

/* The arithmetic every exact promotion step below relies on. */
TEST(expected_steps) {
    static const int e1[] = {1}, e123[] = {1, 2, 3}, e5_30[] = {5, 30}, e31_56[] = {31, 56};

    CHECK_EQ_INT(steps_to_certify(0.2, 0.05 / 4, NULL, 0), 28);
    CHECK_EQ_INT(steps_to_certify(0.2, 0.05 / 4, e1, 1), 40);
    CHECK_EQ_INT(steps_to_certify(0.2, 0.05 / 4, e123, 3), 63);
    CHECK_EQ_INT(steps_to_certify(0.2, 0.05 / 4, e5_30, 2), 52);
    CHECK_EQ_INT(steps_to_certify(0.2, 0.05 / 4, e31_56, 2), 28);
    CHECK_EQ_INT(steps_to_certify(0.2, (0.1 / 6) / 4, NULL, 0), 34);
    CHECK_EQ_INT(steps_to_certify(0.2, (0.1 / 6) / 4, e1, 1), 46);
    CHECK_EQ_INT(steps_to_certify(0.2, 0.05 / 8, NULL, 0), 32);
    CHECK_EQ_INT(steps_to_certify(0.1, 0.05 / 8, NULL, 0), 68);
}

/* gateAllocate() and the constructors the decoder shares with the state
 * machine: a gate assembled by hand from them is a valid gate. */
TEST(constructors) {
    gateConfig c;
    gateGeneration *gen;
    gateObs *o1, *o2, *o3;
    gateSparse x3 = {0, NULL, NULL}, x1 = vec_sparse(&VA);
    static const vec three = {3, {2, 7, 9}, {0.5f, -1.0f, 2.0f}};
    gate *g;
    uint32_t t;

    cfg_classifier(&c);
    c.max_pending = 40;
    CHECK(gateConfigValidate(&c, NULL, 0) == GATE_OK);
    g = gateAllocate(&c);
    CHECK(g != NULL);
    if (g == NULL) exit(1);
    CHECK(g->lineage == 0 && g->epoch == 0 && g->created_ms == 0 && g->now_ms == 0);
    CHECK_EQ_INT(g->next_seq, 1);
    CHECK_EQ_INT(g->table_size, 64); /* smallest power of two >= 40 */
    CHECK_EQ_INT(g->nrows, 1);
    CHECK(g->fixed_bytes == gateFixedBytes(&c));
    CHECK(g->trainer.dim == 16 && g->trainer.nclasses == 2 && g->trainer.updates == 0);
    CHECK(g->grid[0] == 0.5 && g->grid[3] == 0.9999); /* the logit grid of 2 classes */
    CHECK_NEAR(g->grid[1], 0.9556415689811533, 1e-12);
    CHECK(g->bets[0].alpha == 0.2);
    CHECK(g->cfg.teacher != c.teacher && strcmp(g->cfg.teacher, "teacher-v1") == 0); /* deep copy */
    CHECK(g->candidate == NULL && g->serving == NULL && g->oldest == NULL && g->npending == 0);
    CHECK_STATE(g, GATE_STATE_COLD, GATE_REASON_COLD_START);
    INV(g);

    /* An unvalidated configuration is refused instead of half built. */
    c.nthresholds = 1;
    CHECK(gateAllocate(&c) == NULL);
    CHECK(gateCreate(&c, 1, 2, T0) == NULL);
    c.nthresholds = 4;
    CHECK(gateAllocate(NULL) == NULL);

    gen = gateGenerationAllocate(g);
    CHECK(gen != NULL);
    CHECK(gen->gen == 0 && gen->frozen_ms == 0 && gen->promoted_ms == 0 && gen->expires_ms == 0);
    CHECK(gen->samples == 0 && gen->errors == 0 && gen->imputed == 0 && gen->closed == 0);
    CHECK(gen->policy_updates == 0 && gen->regions_tested == 0);
    CHECK(gen->model.w == NULL && gen->model.b == NULL); /* the caller fills it */
    CHECK(gen->policy[0] == -1);
    CHECK(gen->drift[0].n == 0 && gen->drift[0].total == 0);
    for (t = 0; t < 4; t++) {
        CHECK(gen->regions[t].n == 0 && gen->regions[t].certified_at == 0);
        CHECK(gateRegionLogWealth(&gen->regions[t]) == 0.0);
    }
    /* Make it the candidate of generation 1, as the decoder would. */
    CHECK(gateModelCopy(&gen->model, &g->trainer) == GATE_OK);
    g->generations = 1;
    gen->gen = 1;
    gen->regions_tested = 4;
    gen->delta_gen = 0.05;
    gen->delta_region = 0.0125;
    gen->log_threshold = log(80.0);
    g->candidate = gen;
    INV(g);

    o1 = gateObsAllocate();
    o2 = gateObsAllocate();
    o3 = gateObsAllocate();
    CHECK(o1 != NULL && o2 != NULL && o3 != NULL);
    CHECK(o1->result == -1 && o1->cand_label == -1 && o1->serv_label == -1);
    CHECK(o1->seq == 0 && o1->flags == 0 && o1->x.n == 0 && o1->cand_gen == 0 && o1->serv_gen == 0);
    CHECK(o1->prev == NULL && o1->next == NULL && o1->enext == NULL && o1->hnext == NULL);

    x3 = vec_sparse(&three);
    o1->seq = 1;
    o1->flags = GATE_OBS_AUDIT | GATE_OBS_CAND;
    o1->cand_gen = 1;
    o1->cand_label = 1;
    o1->cand_score = 0.75;
    o1->x = x3;
    o2->seq = 2; /* training only */
    o2->cand_gen = 1;
    o2->cand_label = 0;
    o2->cand_score = 0.5;
    o2->x = x1;
    o3->seq = 3;
    o3->flags = GATE_OBS_AUDIT | GATE_OBS_CAND;
    o3->cand_gen = 1;
    o3->cand_label = 0;
    o3->cand_score = 0.6;
    gateObsLink(g, o1);
    gateObsLink(g, o2);
    gateObsLink(g, o3);
    g->next_seq = 4;

    CHECK_EQ_INT(g->npending, 3);
    CHECK_EQ_INT(g->neligible, 2);
    CHECK(g->oldest == o1 && g->newest == o3 && o1->next == o2 && o3->prev == o2);
    CHECK(g->ehead == o1 && g->etail == o3 && o1->enext == o3 && o2->enext == NULL);
    CHECK_EQ_INT(g->pending_bytes, 3 * sizeof(gateObs) + (3 + 1 + 0) * 8);
    CHECK(GATE_OBS_BYTES(3) == sizeof(gateObs) + 3 * (sizeof(uint32_t) + sizeof(float)));
    CHECK(gateFindObservation(g, 1) == o1 && gateFindObservation(g, 2) == o2);
    CHECK(gateFindObservation(g, 3) == o3 && gateFindObservation(g, 4) == NULL);
    CHECK(gateFindObservation(g, 0) == NULL);
    CHECK_EQ_INT(gateFreeEffort(g), 1 + 3 + 1);
    INV(g);

    /* The hand-built gate is a working gate: label the queue head. */
    {
        gateFeedback f = {GATE_FEEDBACK_LABEL, 1, NULL, 0};
        CHECK_FB(gateLearn(g, 5, 0, 1, &f, NULL), GATE_FB_APPLIED);
        CHECK_EQ_INT(g->candidate->samples, 1);
        CHECK_EQ_INT(g->candidate->errors, 0);
        CHECK_EQ_INT(g->npending, 2);
        INV(g);
    }

    gateGenerationRelease(NULL);
    gateObsRelease(NULL);
    gateFree(NULL);
    gateFree(g);
    gateConfigFree(&c);
}

/* 1. COLD -> CALIBRATING -> ACTIVE at the exact step, and the ledger. */
TEST(lifecycle_and_ledger) {
    gateConfig c;
    gatePredictResult r;
    int64_t now = T0, promoted_at;
    uint64_t gen = 99;
    gate *g;
    int i;

    cfg_score(&c);
    g = mk(&c);
    CHECK_STATE(g, GATE_STATE_COLD, GATE_REASON_COLD_START);
    CHECK(g->lineage == LINEAGE && g->epoch == EPOCH && g->created_ms == T0 && g->now_ms == T0);
    CHECK_EQ_INT(g->generations, 0);
    CHECK(gateBudgetSpentNow(g) == 0.0);
    CHECK_NEAR(gateBudgetRemainingNow(g), 0.1, 1e-15);
    CHECK(gateAuditProbability(g) == 0.5); /* COLDAUDIT: nothing is serving */

    /* COLD: even an audited request is evidence for nothing. */
    r = req(g, ++now, 1, 0.9, -1);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_COLD_START);
    CHECK(r.seq == 0 && r.generation == 0 && r.label == -1);

    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    INV(g);
    CHECK_EQ_INT(gen, 1);
    CHECK_STATE(g, GATE_STATE_CALIBRATING, GATE_REASON_COLD_START);
    CHECK_EQ_INT(g->generations, 1);
    CHECK_NEAR(gateBudgetSpentNow(g), 0.05, 1e-15);      /* 0.1 * 1/2 */
    CHECK_NEAR(gateBudgetRemainingNow(g), 0.05, 1e-15);
    CHECK(g->candidate != NULL && g->serving == NULL);
    CHECK(g->candidate->gen == 1 && g->candidate->frozen_ms == now);
    CHECK(g->candidate->promoted_ms == 0 && g->candidate->expires_ms == 0);
    CHECK_NEAR(g->candidate->delta_gen, 0.05, 1e-15);
    CHECK_EQ_INT(g->candidate->regions_tested, 4);
    CHECK_NEAR(g->candidate->delta_region, 0.0125, 1e-15);
    CHECK_NEAR(g->candidate->log_threshold, 4.382026634673881, 1e-12); /* ln 80 */
    CHECK(gateAuditProbability(g) == 0.5);

    /* A second freeze is refused and spends nothing. */
    gen = 99;
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_ERR_CANDIDATE);
    CHECK(gen == 99 && g->generations == 1 && g->candidate->gen == 1);

    /* 27 clean samples are not enough, the 28th is. */
    for (i = 1; i <= 27; i++) {
        sample(g, ++now, 1.0, -1, 1);
        CHECK(g->candidate != NULL && g->candidate->samples == (uint64_t)i);
        CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    }
    CHECK_NEAR(gateRegionLogWealth(&g->candidate->regions[0]), 4.215, 1e-3); /* ln 67.7 */
    r = req(g, ++now, 1, 1.0, -1);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_COLD_START); /* still calibrating here */
    CHECK_FB(fb(g, ++now, r.seq, 1), GATE_FB_APPLIED);
    promoted_at = now;
    CHECK_STATE(g, GATE_STATE_ACTIVE, GATE_REASON_NONE);
    CHECK(g->candidate == NULL && g->serving != NULL);
    CHECK_EQ_INT(g->stats.promotions, 1);
    CHECK_EQ_INT(g->serving->gen, 1);
    CHECK_EQ_INT(g->serving->samples, 28);
    CHECK_EQ_INT(g->serving->errors, 0);
    CHECK_EQ_INT(g->serving->policy[0], 0); /* lowest certified threshold */
    CHECK_EQ_INT(g->serving->policy_updates, 1);
    CHECK_EQ_INT(g->serving->promoted_ms, promoted_at);
    CHECK_EQ_INT(g->serving->expires_ms, promoted_at + CERT_TTL);
    CHECK_EQ_INT(g->serving->closed, 1); /* EXPAND NO: testing stops */
    for (i = 0; i < 4; i++) {
        CHECK_EQ_INT(g->serving->regions[i].n, 28);
        CHECK_EQ_INT(g->serving->regions[i].errors, 0);
        CHECK_EQ_INT(g->serving->regions[i].certified_at, 28);
    }
    CHECK_NEAR(gateRegionLogWealth(&g->serving->regions[0]), 4.40717632572938, 1e-9);
    CHECK(gateGenerationCoverage(g, g->serving) == 1.0);
    CHECK(gateAuditProbability(g) == 0.25); /* AUDIT: a policy is serving */
    CHECK_NEAR(gateBudgetSpentNow(g), 0.05, 1e-15); /* promotion costs nothing more */

    /* Generation 2 in the background: 0.1 / 6, spent 0.1 * 2/3. */
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    INV(g);
    CHECK_EQ_INT(gen, 2);
    CHECK_STATE(g, GATE_STATE_ACTIVE, GATE_REASON_NONE);
    CHECK_NEAR(g->candidate->delta_gen, 0.1 / 6, 1e-15);
    CHECK_NEAR(g->candidate->delta_region, 0.1 / 24, 1e-15);
    CHECK_NEAR(g->candidate->log_threshold, 5.480638923341991, 1e-12); /* ln 240 */
    CHECK_NEAR(gateBudgetSpentNow(g), 0.1 * 2 / 3, 1e-15);
    CHECK_NEAR(gateBudgetRemainingNow(g), 0.1 / 3, 1e-15);

    /* Cancelling it gives neither its number nor its budget back. */
    gateReset(g, ++now, GATE_RESET_CANDIDATE);
    INV(g);
    CHECK(g->candidate == NULL && g->serving != NULL && g->serving->gen == 1);
    CHECK_EQ_INT(g->stats.candidates_discarded, 1);
    CHECK_EQ_INT(g->generations, 2);
    CHECK_NEAR(gateBudgetSpentNow(g), 0.1 * 2 / 3, 1e-15);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    INV(g);
    CHECK_EQ_INT(gen, 3); /* not 2 again */
    CHECK_NEAR(g->candidate->delta_gen, 0.1 / 12, 1e-15);
    CHECK_NEAR(g->candidate->log_threshold, 6.173786103901937, 1e-12); /* ln 480 */
    CHECK_NEAR(gateBudgetSpentNow(g), 0.075, 1e-15); /* 0.1 * 3/4 */
    CHECK_NEAR(gateBudgetRemainingNow(g), 0.025, 1e-15);
    CHECK_NEAR(0.05 + 0.1 / 6 + 0.1 / 12, gateBudgetSpentNow(g), 1e-15); /* sum of the shares */

    /* Neither does any wider reset. */
    gateReset(g, ++now, GATE_RESET_CALIBRATION);
    INV(g);
    CHECK_STATE(g, GATE_STATE_COLD, GATE_REASON_COLD_START);
    CHECK_EQ_INT(g->generations, 3);
    gateReset(g, ++now, GATE_RESET_ALL);
    INV(g);
    CHECK_EQ_INT(g->generations, 3);
    CHECK(g->lineage == LINEAGE);
    CHECK_NEAR(gateBudgetSpentNow(g), 0.075, 1e-15);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    INV(g);
    CHECK_EQ_INT(gen, 4);
    CHECK_NEAR(g->candidate->delta_gen, 0.1 / 20, 1e-15);
    CHECK_NEAR(g->candidate->log_threshold, 6.684611727667927, 1e-12); /* ln 800 */
    CHECK_NEAR(gateBudgetSpentNow(g), 0.08, 1e-15); /* 0.1 * 4/5 */

    /* A later generation needs more evidence: 40 clean samples, not 28. */
    CHECK_EQ_INT(steps_to_certify(0.2, (0.1 / 20) / 4, NULL, 0), 40);
    for (i = 1; i <= 39; i++) sample(g, ++now, 1.0, -1, 1);
    CHECK(g->candidate != NULL && g->serving == NULL);
    sample(g, ++now, 1.0, -1, 1);
    CHECK(g->serving != NULL && g->serving->gen == 4 && g->serving->samples == 40);
    gateFree(g);
}

/* 1. Freeze errors, in the documented order. */
TEST(freeze_errors_in_order) {
    gateConfig c;
    uint64_t gen = 77;
    int64_t now = T0;
    gate *g;

    cfg_classifier(&c);
    g = mk(&c);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_ERR_UNTRAINED);
    CHECK(gen == 77 && g->generations == 0 && g->candidate == NULL);
    INV(g);

    /* Recovery outranks an untrained trainer. */
    gateFence(g, ++now, 1);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_ERR_RECOVERY);
    CHECK(gateRecover(g, ++now, 2) == GATE_OK);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_ERR_UNTRAINED);
    CHECK(gen == 77 && g->generations == 0);

    CHECK(ctrain(g, ++now, &VA, 1) == GATE_OK);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    CHECK(gen == 1 && g->candidate->model.updates == 1);
    INV(g);
    gen = 77;
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_ERR_CANDIDATE);

    /* The API cannot produce a candidate next to an untrained trainer or a
     * pending recovery (both paths discard the candidate), so the order of
     * those checks is pinned by poking the state directly. */
    g->trainer.updates = 0;
    CHECK_EQ_INT(gateFreeze(g, now, &gen), GATE_FREEZE_ERR_CANDIDATE);
    g->recovery_required = 1;
    CHECK_EQ_INT(gateFreeze(g, now, &gen), GATE_FREEZE_ERR_RECOVERY);
    g->recovery_required = 0;
    g->trainer.updates = 1;
    CHECK(gen == 77 && g->generations == 1);
    INV(g);
    gateFree(g);
}

/* 2. Every decision and every counter of a SCORE gate. */
TEST(decisions_and_counters) {
    gateConfig c;
    gatePredictResult r;
    int64_t now = T0;
    uint64_t audit_seq, shadow_seq;
    const gateObs *o;
    gate *g;

    cfg_score(&c);
    g = mk(&c);
    now = promote_at_two_thirds(g, now);
    CHECK_STATE(g, GATE_STATE_ACTIVE, GATE_REASON_NONE);
    CHECK(g->serving->samples == 31 && g->serving->errors == 3);
    CHECK(g->serving->regions[0].n == 31 && g->serving->regions[0].errors == 3);
    CHECK(g->serving->regions[0].certified_at == 0 && g->serving->regions[1].certified_at == 0);
    CHECK(g->serving->regions[2].n == 28 && g->serving->regions[2].certified_at == 28);
    CHECK(g->serving->regions[3].n == 0);
    CHECK_NEAR(gateGenerationCoverage(g, g->serving), 28.0 / 31.0, 1e-15);
    /* Calibration: 31 audited requests, all deferred, all labelled. */
    CHECK_EQ_INT(g->stats.requests, 31);
    CHECK_EQ_INT(g->stats.abstained, 31);
    CHECK_EQ_INT(g->stats.abstain_reason[GATE_REASON_COLD_START], 31);
    CHECK_EQ_INT(g->stats.observations, 31);
    CHECK_EQ_INT(g->stats.eligible, 31);
    CHECK_EQ_INT(g->stats.feedback[GATE_FB_APPLIED], 31);
    CHECK_EQ_INT(g->stats.would_accept, 0);

    /* Above the threshold, audit bit clear: ACCEPT, and nothing to learn. */
    r = req(g, ++now, 0, 0.9, -1);
    CHECK_RES(r, GATE_ACTION_ACCEPT, GATE_REASON_NONE);
    CHECK(r.label == -1 && r.score == 0.9 && r.generation == 1 && r.risk_target == 0.2);
    CHECK(r.seq == 0 && g->npending == 0);
    /* The threshold itself is inside the region, one ulp below is not. */
    r = req(g, ++now, 0, g->grid[2], -1);
    CHECK_RES(r, GATE_ACTION_ACCEPT, GATE_REASON_NONE);
    r = req(g, ++now, 0, nextafter(g->grid[2], 0.0), -1);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_BELOW_THRESHOLD);
    CHECK(r.seq == 0 && r.generation == 1 && r.label == -1 && r.score == 0.0);

    /* Above the threshold, audit bit set: AUDIT. */
    r = req(g, ++now, 1, 0.9, -1);
    CHECK_RES(r, GATE_ACTION_AUDIT, GATE_REASON_NONE);
    CHECK(r.seq == 32 && r.generation == 1 && r.score == 0.9 && r.label == -1);
    CHECK(r.risk_target == 0.0);
    audit_seq = r.seq;
    o = gateFindObservation(g, audit_seq);
    CHECK(o != NULL && o->flags == (GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_SERV));
    CHECK(o != NULL && o->serv_gen == 1 && o->serv_score == 0.9 && o->cand_gen == 0);

    /* Below the threshold with the audit bit set: the serving generation
     * stopped testing and there is no candidate, so nobody wants the label. */
    r = req(g, ++now, 1, 0.5, -1);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_BELOW_THRESHOLD);
    CHECK(r.seq == 0);

    /* Paused: nothing is accepted, whatever the score; audits continue. */
    gatePause(g, ++now);
    INV(g);
    CHECK_STATE(g, GATE_STATE_PAUSED, GATE_REASON_PAUSED);
    r = req(g, ++now, 0, 0.9, -1);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_PAUSED);
    CHECK(r.seq == 0 && r.generation == 1);
    r = req(g, ++now, 1, 0.9, -1);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_PAUSED);
    CHECK(r.seq == 33);
    shadow_seq = r.seq;
    o = gateFindObservation(g, shadow_seq);
    CHECK(o != NULL && o->flags == (GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_SERV));
    r = req(g, ++now, 0, 0.5, -1);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_PAUSED); /* not BELOW_THRESHOLD */
    gatePause(g, ++now); /* idempotent */
    CHECK(g->paused == 1);
    gateResume(g, ++now);
    gateResume(g, ++now);
    INV(g);
    CHECK_STATE(g, GATE_STATE_ACTIVE, GATE_REASON_NONE);
    r = req(g, ++now, 0, 0.9, -1);
    CHECK_RES(r, GATE_ACTION_ACCEPT, GATE_REASON_NONE);

    /* Both audits, the paused one included, reach the drift window. */
    CHECK_FB(fb(g, ++now, audit_seq, 1), GATE_FB_APPLIED);
    CHECK_FB(fb(g, ++now, shadow_seq, 0), GATE_FB_APPLIED);
    CHECK(g->serving->drift[0].total == 2 && g->serving->drift[0].total_errors == 1);
    CHECK(g->serving->drift[0].n == 2 && g->serving->drift[0].errors == 1);
    CHECK(g->serving->samples == 31); /* but no region: the policy is frozen */

    CHECK_EQ_INT(g->stats.requests, 40);
    CHECK_EQ_INT(g->stats.accepted, 3);
    CHECK_EQ_INT(g->stats.audited, 1);
    CHECK_EQ_INT(g->stats.abstained, 36);
    CHECK_EQ_INT(g->stats.would_accept, 6); /* 3 accepted, 1 audited, 2 paused */
    CHECK_EQ_INT(g->stats.abstain_reason[GATE_REASON_COLD_START], 31);
    CHECK_EQ_INT(g->stats.abstain_reason[GATE_REASON_BELOW_THRESHOLD], 2);
    CHECK_EQ_INT(g->stats.abstain_reason[GATE_REASON_PAUSED], 3);
    CHECK_EQ_INT(g->stats.observations, 33);
    CHECK_EQ_INT(g->stats.eligible, 33);
    CHECK_EQ_INT(g->stats.feedback[GATE_FB_APPLIED], 33);
    gateFree(g);
}

/* 2. A paused gate keeps calibrating, gets promoted, and resumes ACTIVE. */
TEST(paused_gate_keeps_calibrating) {
    gateConfig c;
    gatePredictResult r;
    int64_t now = T0;
    uint64_t gen;
    gate *g;
    int i;

    cfg_score(&c);
    g = mk(&c);
    gatePause(g, ++now);
    CHECK_STATE(g, GATE_STATE_PAUSED, GATE_REASON_PAUSED);
    r = req(g, ++now, 1, 1.0, -1);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_PAUSED);
    CHECK(r.seq == 0);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    CHECK_STATE(g, GATE_STATE_PAUSED, GATE_REASON_PAUSED);
    for (i = 0; i < 28; i++) {
        r = req(g, ++now, 1, 1.0, -1);
        CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_PAUSED);
        CHECK(r.seq != 0);
        CHECK_FB(fb(g, now, r.seq, 1), GATE_FB_APPLIED);
    }
    CHECK(g->serving != NULL && g->candidate == NULL && g->stats.promotions == 1);
    CHECK_STATE(g, GATE_STATE_PAUSED, GATE_REASON_PAUSED);
    r = req(g, ++now, 0, 1.0, -1);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_PAUSED);
    CHECK(r.generation == 1 && g->stats.would_accept == 1 && g->stats.accepted == 0);
    gateResume(g, ++now);
    CHECK_STATE(g, GATE_STATE_ACTIVE, GATE_REASON_NONE);
    r = req(g, ++now, 0, 1.0, -1);
    CHECK_RES(r, GATE_ACTION_ACCEPT, GATE_REASON_NONE);
    gateFree(g);
}

/* 3. SCORE mode: an observation exists only for eligible requests. */
TEST(observation_rules_score) {
    gateConfig c;
    gatePredictResult r;
    int64_t now = T0;
    const gateObs *o;
    uint64_t gen;
    gate *g;

    cfg_score(&c);
    g = mk(&c);
    r = req(g, ++now, 1, 0.7, -1); /* COLD */
    CHECK(r.seq == 0);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    r = req(g, ++now, 0, 0.7, -1); /* CALIBRATING, audit bit clear */
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_COLD_START);
    CHECK(r.seq == 0 && g->npending == 0);
    r = req(g, ++now, 1, 0.7, -1); /* CALIBRATING, audit bit set */
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_COLD_START);
    CHECK(r.seq == 1 && r.generation == 0);
    o = gateFindObservation(g, 1);
    CHECK(o != NULL);
    if (o != NULL) {
        CHECK(o->flags == (GATE_OBS_AUDIT | GATE_OBS_CAND));
        CHECK(o->created_ms == now && o->result == -1 && o->x.n == 0);
        CHECK(o->cand_gen == 1 && o->cand_label == -1 && o->cand_score == 0.7);
        CHECK(o->serv_gen == 0 && o->serv_label == -1 && o->serv_score == 0.0);
    }
    CHECK(g->npending == 1 && g->neligible == 1 && g->pending_bytes == sizeof(gateObs));
    CHECK(g->stats.observations == 1 && g->stats.eligible == 1 && g->stats.requests == 3);
    CHECK_EQ_INT(gateOldestPendingAge(g), 0);
    gateFree(g);

    /* ACTIVE, with and without a background candidate. */
    cfg_score(&c);
    g = mk(&c);
    now = promote_at_two_thirds(g, T0);
    r = req(g, ++now, 1, 0.5, -1);
    CHECK(r.seq == 0);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    CHECK(gen == 2);
    r = req(g, ++now, 0, 0.5, -1);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_BELOW_THRESHOLD);
    CHECK(r.seq == 0);
    r = req(g, ++now, 0, 0.9, -1);
    CHECK_RES(r, GATE_ACTION_ACCEPT, GATE_REASON_NONE);
    r = req(g, ++now, 1, 0.5, -1); /* sampled for the candidate only */
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_BELOW_THRESHOLD);
    CHECK(r.seq != 0 && r.generation == 1);
    o = gateFindObservation(g, r.seq);
    CHECK(o != NULL && o->flags == (GATE_OBS_AUDIT | GATE_OBS_CAND));
    CHECK(o != NULL && o->cand_gen == 2 && o->cand_score == 0.5);
    CHECK(o != NULL && o->serv_gen == 1 && o->serv_score == 0.5); /* recorded, not evidence */
    r = req(g, ++now, 1, 0.9, -1); /* sampled for both */
    CHECK_RES(r, GATE_ACTION_AUDIT, GATE_REASON_NONE);
    o = gateFindObservation(g, r.seq);
    CHECK(o != NULL && o->flags == (GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_CAND |
                                    GATE_OBS_SERV));
    CHECK(o != NULL && o->cand_gen == 2 && o->serv_gen == 1);
    CHECK(g->npending == 2 && g->neligible == 2);
    gateFree(g);
}

/* 3. CLASSIFIER mode: an observation for every request that is not an
 * ACCEPT, the ownership of the features, gatePredictNeedsFeatures(). */
TEST(observation_rules_classifier) {
    gateConfig c;
    gatePredictResult r;
    int64_t now = T0;
    const gateObs *o;
    uint64_t first, fp;
    gateSparse x;
    gateEval ev;
    double sa, sc;
    gate *g;
    int lc;

    cfg_classifier(&c);
    g = mk(&c);

    /* COLD and untrained: the label is still wanted, to train. */
    r = creq(g, ++now, 0, &VA);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_COLD_START);
    CHECK(r.seq == 1);
    first = r.seq;
    o = gateFindObservation(g, 1);
    CHECK(o != NULL && o->flags == 0 && o->x.n == 1 && o->x.idx[0] == 0 && o->x.val[0] == 1.0f);
    CHECK(o != NULL && o->cand_gen == 0 && o->serv_gen == 0 && o->cand_label == -1);
    r = creq(g, ++now, 1, &VB); /* audited, but there is nothing to audit */
    o = gateFindObservation(g, r.seq);
    CHECK(o != NULL && o->flags == GATE_OBS_AUDIT);
    CHECK(g->npending == 2 && g->neligible == 0);
    CHECK(g->stats.observations == 2 && g->stats.eligible == 0);
    CHECK_EQ_INT(g->pending_bytes, 2 * GATE_OBS_BYTES(1));

    /* The features are needed and not supplied: refused, nothing changes. */
    gateMaintain(g, now);
    CHECK(gateEvaluate(g, NULL, 0.0, -1, &ev) == GATE_OK); /* no generation: x not needed */
    CHECK(gatePredictNeedsFeatures(g, 0, &ev) == 1);
    fp = fingerprint(g, FP_ALL);
    CHECK(gatePredict(g, now, 0, &ev, NULL, &r) == GATE_ERR);
    CHECK(fingerprint(g, FP_ALL) == fp);
    INV(g);

    /* Feedback trains, releases the observation and its bytes. */
    CHECK_FB(clearn(g, ++now, first, 1), GATE_FB_APPLIED);
    CHECK(g->trainer.updates == 1 && g->npending == 1);
    CHECK_EQ_INT(g->pending_bytes, GATE_OBS_BYTES(1));
    CHECK(gateFindObservation(g, first) == NULL);

    /* ACTIVE with the policy "score >= 0.9556". */
    now = classifier_promote(g, now);
    lc = model_says(&g->serving->model, &VC, &sc);
    CHECK(model_says(&g->serving->model, &VA, &sa) == 1);

    r = creq(g, ++now, 0, &VA);
    CHECK_RES(r, GATE_ACTION_ACCEPT, GATE_REASON_NONE);
    CHECK(r.label == 1 && r.score == sa && r.generation == 1 && r.risk_target == 0.2);
    CHECK(r.seq == 0);
    /* An ACCEPT needs no features at all. */
    x = vec_sparse(&VA);
    gateMaintain(g, ++now);
    CHECK(gateEvaluate(g, &x, 0.0, -1, &ev) == GATE_OK);
    gateSparseFree(&x);
    CHECK(gatePredictNeedsFeatures(g, 0, &ev) == 0);
    CHECK(gatePredictNeedsFeatures(g, 1, &ev) == 1);
    CHECK(gatePredict(g, now, 0, &ev, NULL, &r) == GATE_OK);
    CHECK_RES(r, GATE_ACTION_ACCEPT, GATE_REASON_NONE);
    CHECK(gatePredict(g, now, 1, &ev, NULL, &r) == GATE_ERR); /* the AUDIT would store them */
    INV(g);

    r = creq(g, ++now, 1, &VA);
    CHECK_RES(r, GATE_ACTION_AUDIT, GATE_REASON_NONE);
    CHECK(r.label == 1 && r.score == sa && r.generation == 1);
    o = gateFindObservation(g, r.seq);
    CHECK(o != NULL && o->flags == (GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_SERV));
    CHECK(o != NULL && o->serv_label == 1 && o->serv_score == sa && o->serv_gen == 1);

    r = creq(g, ++now, 0, &VC); /* below the threshold: a training observation */
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_BELOW_THRESHOLD);
    CHECK(r.seq != 0 && r.generation == 1 && r.label == -1);
    o = gateFindObservation(g, r.seq);
    CHECK(o != NULL && o->flags == 0 && o->serv_label == lc && o->serv_score == sc);
    r = creq(g, ++now, 1, &VC); /* audited, but the generation stopped testing */
    o = gateFindObservation(g, r.seq);
    CHECK(o != NULL && o->flags == GATE_OBS_AUDIT);

    /* Shadow mode still collects training labels for would-accepts. */
    gatePause(g, ++now);
    r = creq(g, ++now, 0, &VA);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_PAUSED);
    o = gateFindObservation(g, r.seq);
    CHECK(o != NULL && o->flags == GATE_OBS_WOULD_ACCEPT);
    gateResume(g, ++now);

    /* Awaiting recovery: no observation, not even for training. */
    gateFence(g, ++now, 7);
    INV(g);
    r = creq(g, ++now, 1, &VA);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_RECOVERY_REQUIRED);
    CHECK(r.seq == 0 && r.generation == 0 && g->npending == 0);
    CHECK_EQ_INT(g->stats.abstain_reason[GATE_REASON_RECOVERY_REQUIRED], 1);
    gateFree(g);
}

/* 4. Evidence comes from the predictions frozen at GPREDICT time. */
TEST(evidence_uses_frozen_predictions) {
    gateConfig c;
    gatePredictResult r;
    int64_t now = T0;
    uint64_t gen, early, o1, o2, updates;
    gateRegion regions[4];
    float frozen_w[16], frozen_b;
    const gateObs *o;
    double s;
    gate *g;
    int i;

    cfg_classifier(&c);
    g = mk(&c);
    r = creq(g, ++now, 1, &VA); /* predicted before any freeze */
    early = r.seq;
    train_ab(g, now);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    memcpy(frozen_w, g->candidate->model.w, sizeof(frozen_w));
    frozen_b = g->candidate->model.b[0];
    CHECK(g->candidate->model.w != g->trainer.w); /* a copy, not a reference */

    /* Labelled after the freeze: it trains, it is not evidence. */
    updates = g->trainer.updates;
    CHECK_FB(clearn(g, ++now, early, 1), GATE_FB_APPLIED);
    CHECK(g->trainer.updates == updates + 1 && g->candidate->samples == 0);

    /* o1: the candidate says "yes". */
    r = creq(g, ++now, 1, &VA);
    o1 = r.seq;
    o = gateFindObservation(g, o1);
    CHECK(o != NULL && o->flags == (GATE_OBS_AUDIT | GATE_OBS_CAND));
    CHECK(o != NULL && o->cand_label == 1 && o->cand_gen == 1);

    /* The trainer is now taught the opposite until it says "no" for A.
     * GADD is never evidence and never reaches the frozen copy. */
    memcpy(regions, g->candidate->regions, sizeof(regions));
    for (i = 0; i < 2000 && model_says(&g->trainer, &VA, &s) != 0; i++)
        CHECK(ctrain(g, now, &VA, 0) == GATE_OK);
    CHECK(model_says(&g->trainer, &VA, &s) == 0);
    CHECK(model_says(&g->candidate->model, &VA, &s) == 1);
    CHECK(g->candidate->samples == 0);
    CHECK(memcmp(regions, g->candidate->regions, sizeof(regions)) == 0);
    CHECK(memcmp(frozen_w, g->candidate->model.w, sizeof(frozen_w)) == 0);

    /* The teacher says "no". The trainer would agree today; the prediction
     * that was actually made did not: one error. */
    CHECK_FB(clearn(g, ++now, o1, 0), GATE_FB_APPLIED);
    CHECK(g->candidate->samples == 1 && g->candidate->errors == 1);

    /* o2: the candidate still says "yes", the trainer "no"; the teacher says
     * "yes". Recomputing with the trainer would count an error. */
    r = creq(g, ++now, 1, &VA);
    o2 = r.seq;
    o = gateFindObservation(g, o2);
    CHECK(o != NULL && o->cand_label == 1);
    CHECK(model_says(&g->trainer, &VA, &s) == 0);
    CHECK_FB(clearn(g, ++now, o2, 1), GATE_FB_APPLIED);
    CHECK(g->candidate->samples == 2 && g->candidate->errors == 1);
    CHECK(g->candidate->regions[0].n == 2 && g->candidate->regions[0].errors == 1);

    /* Feedback moved the trainer, never the frozen weights. */
    CHECK(memcmp(frozen_w, g->candidate->model.w, sizeof(frozen_w)) == 0);
    CHECK(frozen_b == g->candidate->model.b[0]);
    CHECK(g->candidate->model.updates == 800); /* as frozen: 400 rounds of two */
    gateFree(g);
}

/* 4. An observation is evidence only for the generations it was sampled
 * for: not for one frozen later, not for the successor of a cancelled one. */
TEST(evidence_is_bound_to_its_generation) {
    gateConfig c;
    gatePredictResult r;
    int64_t now = T0;
    uint64_t gen, head, queued, lone;
    const gateObs *o;
    gate *g;

    cfg_score(&c);
    g = mk(&c);
    now = promote_at_two_thirds(g, now); /* generation 1 serves, closed */
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK); /* generation 2 */
    r = req(g, ++now, 1, 0.9, -1); /* AUDIT: for 1 (serving) and 2 (candidate) */
    head = r.seq;
    r = req(g, ++now, 1, 0.5, -1); /* for 2 only */
    queued = r.seq;

    gateReset(g, ++now, GATE_RESET_CANDIDATE);
    INV(g);
    /* 'queued' is evidence for nobody now, but it is behind 'head', which
     * generation 1 still waits for: it keeps its place and its flags. */
    CHECK(g->npending == 2 && g->neligible == 2);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    CHECK(gen == 3);

    CHECK_FB(fb(g, ++now, queued, 1), GATE_FB_APPLIED); /* buffered behind 'head' */
    CHECK(g->candidate->samples == 0 && g->npending == 2);
    CHECK_FB(fb(g, ++now, head, 1), GATE_FB_APPLIED);
    CHECK(g->npending == 0 && g->neligible == 0);
    CHECK(g->serving->drift[0].total == 1); /* 'head' audited generation 1 */
    CHECK(g->candidate->gen == 3 && g->candidate->samples == 0); /* 3 got neither */

    /* Alone in the queue, an orphaned observation is demoted at once. */
    r = req(g, ++now, 1, 0.5, -1);
    lone = r.seq;
    gateReset(g, ++now, GATE_RESET_CANDIDATE);
    INV(g);
    o = gateFindObservation(g, lone);
    CHECK(o != NULL && o->flags == GATE_OBS_AUDIT && o->cand_gen == 3);
    CHECK(g->npending == 1 && g->neligible == 0);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    CHECK(gen == 4);
    CHECK_FB(fb(g, ++now, lone, 1), GATE_FB_APPLIED); /* consumed, with no effect */
    CHECK(g->candidate->samples == 0 && g->npending == 0);
    gateFree(g);
}

/* 5. Out-of-order feedback is buffered until every older eligible
 * observation is labelled. */
TEST(evidence_waits_for_older_observations) {
    gateConfig c;
    int64_t now = T0;
    uint64_t gen, o1, o2, o3, e1, tr, e2;
    const gateObs *o;
    gate *g;

    cfg_score(&c);
    g = mk(&c);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    o1 = req(g, ++now, 1, 0.9, -1).seq;
    o2 = req(g, ++now, 1, 0.9, -1).seq;
    o3 = req(g, ++now, 1, 0.5, -1).seq;

    CHECK_FB(fb(g, ++now, o3, 0), GATE_FB_APPLIED);
    CHECK(g->candidate->samples == 0 && g->candidate->errors == 0);
    o = gateFindObservation(g, o3);
    CHECK(o != NULL && (o->flags & GATE_OBS_RESOLVED) && o->result == 0);
    CHECK(g->npending == 3 && g->neligible == 3);
    CHECK_FB(fb(g, ++now, o2, 1), GATE_FB_APPLIED);
    CHECK(g->candidate->samples == 0 && g->npending == 3);

    CHECK_FB(fb(g, ++now, o1, 1), GATE_FB_APPLIED); /* releases all three, in order */
    CHECK(g->candidate->samples == 3 && g->candidate->errors == 1);
    CHECK(g->npending == 0 && g->neligible == 0 && g->pending_bytes == 0);
    CHECK(g->candidate->regions[0].n == 3 && g->candidate->regions[0].errors == 1);
    CHECK(g->candidate->regions[1].n == 3 && g->candidate->regions[1].errors == 1);
    CHECK(g->candidate->regions[2].n == 2 && g->candidate->regions[2].errors == 0);
    CHECK(g->candidate->regions[3].n == 0);
    gateFree(g);

    /* A training-only observation is not part of the queue: it neither
     * waits nor makes anything wait. */
    cfg_classifier(&c);
    g = mk(&c);
    CHECK(ctrain(g, ++now, &VA, 1) == GATE_OK);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    e1 = creq(g, ++now, 1, &VA).seq;
    tr = creq(g, ++now, 0, &VB).seq;
    e2 = creq(g, ++now, 1, &VA).seq;
    CHECK(g->npending == 3 && g->neligible == 2);
    CHECK_FB(clearn(g, ++now, e2, 1), GATE_FB_APPLIED);
    CHECK(g->candidate->samples == 0 && g->npending == 3);
    /* Its features were consumed by the SGD step; only the stub waits. */
    CHECK_EQ_INT(g->pending_bytes, 2 * GATE_OBS_BYTES(1) + GATE_OBS_BYTES(0));
    CHECK_FB(clearn(g, ++now, tr, 0), GATE_FB_APPLIED);
    CHECK(g->candidate->samples == 0 && g->npending == 2);
    CHECK_FB(clearn(g, ++now, e1, 1), GATE_FB_APPLIED);
    CHECK(g->candidate->samples == 2 && g->npending == 0 && g->pending_bytes == 0);
    CHECK_EQ_INT(g->trainer.updates, 1 + 3);
    gateFree(g);
}

/* 5. The calibration stream is the prediction order, whatever the order in
 * which the teacher answers. */
#define STREAM 60
static int stream_correct(int i) { return i != 5 && i != 30; }

static gate *run_stream(const int *arrival) {
    gateConfig c;
    uint64_t gen, seq[STREAM + 1];
    gate *g;
    int i;

    cfg_score(&c);
    g = mk(&c);
    CHECK_EQ_INT(gateFreeze(g, T0 + 1, &gen), GATE_FREEZE_OK);
    for (i = 1; i <= STREAM; i++) seq[i] = req(g, T0 + 1 + i, 1, 1.0, -1).seq;
    CHECK(g->npending == STREAM && g->neligible == STREAM);
    for (i = 0; i < STREAM; i++)
        CHECK_FB(fb(g, T0 + 100, seq[arrival[i]], stream_correct(arrival[i])), GATE_FB_APPLIED);
    return g;
}

TEST(arrival_order_does_not_select_the_stream) {
    int forward[STREAM], backward[STREAM], shuffled[STREAM], i;
    uint64_t rng = 20240607;
    gate *a, *b, *s;

    for (i = 0; i < STREAM; i++) {
        forward[i] = i + 1;
        backward[i] = STREAM - i;
        shuffled[i] = i + 1;
    }
    for (i = STREAM - 1; i > 0; i--) {
        int j = (int)(gate_test_rand64(&rng) % (uint64_t)(i + 1)), tmp = shuffled[i];
        shuffled[i] = shuffled[j];
        shuffled[j] = tmp;
    }
    a = run_stream(forward);
    b = run_stream(backward);
    s = run_stream(shuffled);

    /* Errors at positions 5 and 30: the region certifies on sample 52.
     * Applied in the backward arrival order the errors would sit at 31 and
     * 56 and the first 28 samples would certify: the two differ. */
    CHECK(a->serving != NULL && b->serving != NULL && s->serving != NULL);
    if (a->serving != NULL && b->serving != NULL && s->serving != NULL) {
        CHECK_EQ_INT(a->serving->samples, 52);
        CHECK_EQ_INT(b->serving->samples, 52);
        CHECK_EQ_INT(s->serving->samples, 52);
        CHECK_EQ_INT(a->serving->errors, 2);
        CHECK_EQ_INT(b->serving->regions[0].certified_at, 52);
        CHECK(memcmp(a->serving->regions, b->serving->regions, 4 * sizeof(gateRegion)) == 0);
        CHECK(memcmp(a->serving->regions, s->serving->regions, 4 * sizeof(gateRegion)) == 0);
    }
    /* Not just the regions: the three gates are in the same state. */
    CHECK(fingerprint(a, FP_ALL) == fingerprint(b, FP_ALL));
    CHECK(fingerprint(a, FP_ALL) == fingerprint(s, FP_ALL));
    CHECK(a->npending == 0 && b->npending == 0 && s->npending == 0);
    gateFree(a);
    gateFree(b);
    gateFree(s);
}

/* 6. Every feedback status; only APPLIED changes anything. */
TEST(feedback_statuses) {
    gateConfig c;
    gateFeedback f = {GATE_FEEDBACK_CORRECT, -1, NULL, 1};
    int64_t now = T0, t4;
    uint64_t gen, o1, o2, o3, o4, o5, o6, o7, fp;
    const gateObs *o;
    gate *g;

    cfg_score(&c);
    g = mk(&c);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    o1 = req(g, ++now, 1, 0.9, -1).seq;
    o2 = req(g, ++now, 1, 0.9, -1).seq;
    o3 = req(g, ++now, 1, 0.9, -1).seq;
    CHECK(o1 == 1 && o2 == 2 && o3 == 3);

    CHECK_FB(fb(g, now, o1, 1), GATE_FB_APPLIED);
    CHECK(g->candidate->samples == 1);
    fp = fingerprint(g, FP_NO_FEEDBACK);
    CHECK_FB(fb(g, now, o1, 1), GATE_FB_DUPLICATE); /* consumed */
    CHECK(fingerprint(g, FP_NO_FEEDBACK) == fp);

    CHECK_FB(fb(g, now, o3, 1), GATE_FB_APPLIED); /* waits behind o2 */
    CHECK(g->candidate->samples == 1);
    fp = fingerprint(g, FP_NO_FEEDBACK);
    CHECK_FB(fb(g, now, o3, 0), GATE_FB_DUPLICATE); /* pending, but resolved */
    CHECK(fingerprint(g, FP_NO_FEEDBACK) == fp);
    o = gateFindObservation(g, o3);
    CHECK(o != NULL && o->result == 1); /* the second verdict did not overwrite */

    /* Not an id of this gate's current epoch. */
    CHECK_FB(gateLearn(g, now, g->epoch ^ 1, o2, &f, NULL), GATE_FB_UNKNOWN);
    CHECK_FB(gateLearn(g, now, g->epoch, 0, &f, NULL), GATE_FB_UNKNOWN);
    CHECK_FB(gateLearn(g, now, g->epoch, g->next_seq, &f, NULL), GATE_FB_UNKNOWN);
    CHECK_FB(gateLearn(g, now, g->epoch, 1000, &f, NULL), GATE_FB_UNKNOWN);
    CHECK(fingerprint(g, FP_NO_FEEDBACK) == fp);

    /* A verdict from another teacher leaves the observation pending. */
    CHECK_FB(gateLearn(g, now, g->epoch, o2, &f, "teacher-v2"), GATE_FB_TEACHER_MISMATCH);
    CHECK(fingerprint(g, FP_NO_FEEDBACK) == fp);
    o = gateFindObservation(g, o2);
    CHECK(o != NULL && !(o->flags & GATE_OBS_RESOLVED) && o->result == -1);
    CHECK_FB(gateLearn(g, now, g->epoch, o2, &f, "teacher-v1"), GATE_FB_APPLIED);
    INV(g);
    CHECK(g->candidate->samples == 3 && g->npending == 0); /* o2, then o3 */

    CHECK_EQ_INT(g->stats.feedback[GATE_FB_APPLIED], 3);
    CHECK_EQ_INT(g->stats.feedback[GATE_FB_DUPLICATE], 2);
    CHECK_EQ_INT(g->stats.feedback[GATE_FB_UNKNOWN], 4);
    CHECK_EQ_INT(g->stats.feedback[GATE_FB_TEACHER_MISMATCH], 1);

    /* Too late: the TTL elapsed exactly at created + TTL. */
    t4 = ++now;
    o4 = req(g, t4, 1, 0.9, -1).seq;
    CHECK_FB(fb(g, t4 + TTL, o4, 1), GATE_FB_EXPIRED);
    CHECK(g->expired_through == o4 && g->npending == 0);
    CHECK_EQ_INT(g->stats.feedback[GATE_FB_EXPIRED], 1);
    /* Consumed ids are not remembered one by one: below the expiry
     * watermark a consumed id reports EXPIRED as well. */
    CHECK_FB(fb(g, t4 + TTL, o1, 1), GATE_FB_EXPIRED);
    now = t4 + TTL;

    /* Dropped by GRESET ALL (which also zeroes the counters). */
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    o5 = req(g, ++now, 1, 0.9, -1).seq;
    gateReset(g, ++now, GATE_RESET_ALL);
    INV(g);
    CHECK(g->invalid_below == g->next_seq && g->npending == 0);
    fp = fingerprint(g, FP_NO_FEEDBACK);
    CHECK_FB(fb(g, now, o5, 1), GATE_FB_INVALIDATED);
    CHECK_FB(fb(g, now, o1, 1), GATE_FB_INVALIDATED); /* outranks EXPIRED */
    CHECK(fingerprint(g, FP_NO_FEEDBACK) == fp);
    CHECK_EQ_INT(g->stats.feedback[GATE_FB_INVALIDATED], 2);
    CHECK_EQ_INT(g->stats.feedback[GATE_FB_APPLIED], 0);

    /* Dropped by a teacher change; the old teacher is a mismatch now. */
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    o6 = req(g, ++now, 1, 0.9, -1).seq;
    CHECK(gateSetTeacher(g, ++now, "teacher-v2") == GATE_OK);
    INV(g);
    CHECK_FB(fb(g, now, o6, 1), GATE_FB_INVALIDATED);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    o7 = req(g, ++now, 1, 0.9, -1).seq;
    CHECK_FB(gateLearn(g, now, g->epoch, o7, &f, "teacher-v1"), GATE_FB_TEACHER_MISMATCH);
    CHECK_FB(gateLearn(g, now, g->epoch, o7, &f, "teacher-v2"), GATE_FB_APPLIED);
    INV(g);
    CHECK(g->candidate->samples == 1);
    gateFree(g);
}

/* 7. ONMISSING REVOKE: a missing label invalidates what it belonged to. */
TEST(missing_label_revoke) {
    gateConfig c;
    gatePredictResult r;
    gateStateId st;
    gateReason why;
    gateEval stale;
    int64_t now = T0, t;
    uint64_t gen, o1, o2;
    gate *g;

    /* (a) A calibration sample: the candidate is discarded, at the TTL and
     * not one millisecond earlier; its budget stays spent. */
    cfg_score(&c);
    g = mk(&c);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    t = ++now;
    o1 = req(g, t, 1, 0.9, -1).seq;
    gateMaintain(g, t + TTL - 1);
    INV(g);
    CHECK(g->candidate != NULL && g->npending == 1);
    CHECK_EQ_INT(gateOldestPendingAge(g), TTL - 1);
    gatePreview(g, t + TTL, 0, &st, &why); /* GINFO sees it coming */
    CHECK(st == GATE_STATE_COLD && why == GATE_REASON_COLD_START);
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    gateMaintain(g, t + TTL);
    INV(g);
    CHECK(g->candidate == NULL && g->npending == 0);
    CHECK_STATE(g, GATE_STATE_COLD, GATE_REASON_COLD_START);
    CHECK_EQ_INT(g->stats.candidates_discarded, 1);
    CHECK_EQ_INT(g->stats.expired, 1);
    CHECK_EQ_INT(g->stats.expired_eligible, 1);
    CHECK_EQ_INT(g->stats.imputed, 0);
    CHECK_EQ_INT(g->generations, 1);
    CHECK_NEAR(gateBudgetSpentNow(g), 0.05, 1e-15);
    CHECK_FB(fb(g, t + TTL, o1, 1), GATE_FB_EXPIRED);
    gateFree(g);

    /* (b) The same while a generation is serving: serving is untouched. */
    cfg_score(&c);
    g = mk(&c);
    now = promote_at_two_thirds(g, T0);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    t = ++now;
    r = req(g, t, 1, 0.5, -1); /* below the threshold: for the candidate only */
    CHECK(r.seq != 0);
    gateMaintain(g, t + TTL);
    INV(g);
    CHECK(g->candidate == NULL && g->serving != NULL && g->serving->gen == 1);
    CHECK_STATE(g, GATE_STATE_ACTIVE, GATE_REASON_NONE);
    CHECK(g->stats.revocations == 0 && g->stats.candidates_discarded == 1);
    gateFree(g);

    /* (c) An AUDIT: serving is revoked, and the very next GPREDICT, even
     * one evaluated while the policy was still valid, cannot accept. */
    cfg_score(&c);
    g = mk(&c);
    now = promote_at_two_thirds(g, T0);
    t = ++now;
    r = req(g, t, 1, 0.9, -1);
    CHECK_RES(r, GATE_ACTION_AUDIT, GATE_REASON_NONE);
    r = req(g, t + TTL - 1, 0, 0.9, -1);
    CHECK_RES(r, GATE_ACTION_ACCEPT, GATE_REASON_NONE); /* the last valid millisecond */
    CHECK(gateEvaluate(g, NULL, 0.9, -1, &stale) == GATE_OK);
    CHECK(stale.has_serving == 1);
    gatePreview(g, t + TTL, 0, &st, &why);
    CHECK(st == GATE_STATE_REVOKED && why == GATE_REASON_FEEDBACK_INTEGRITY);
    CHECK(gateState(g) == GATE_STATE_ACTIVE);
    CHECK(gatePredict(g, t + TTL, 0, &stale, NULL, &r) == GATE_OK);
    INV(g);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_FEEDBACK_INTEGRITY);
    CHECK(r.generation == 0 && r.seq == 0);
    CHECK_STATE(g, GATE_STATE_REVOKED, GATE_REASON_FEEDBACK_INTEGRITY);
    CHECK(g->serving == NULL && g->stats.revocations == 1 && g->stats.expired_eligible == 1);
    CHECK(g->last_revoked_reason == GATE_REASON_FEEDBACK_INTEGRITY);
    CHECK(g->last_revoked_gen == 1 && g->last_revoked_ms == t + TTL);
    CHECK_EQ_INT(g->stats.accepted, 1);
    gateFree(g);

    /* (d) EXPAND YES, a testing sample outside the policy: the certified
     * policy keeps serving, only its testing is closed. */
    cfg_score(&c);
    c.expand = 1;
    g = mk(&c);
    now = promote_at_two_thirds(g, T0);
    CHECK(g->serving->closed == 0);
    t = ++now;
    r = req(g, t, 1, 0.5, -1);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_BELOW_THRESHOLD);
    CHECK(r.seq != 0);
    CHECK(gateFindObservation(g, r.seq)->flags == (GATE_OBS_AUDIT | GATE_OBS_SERV));
    gatePreview(g, t + TTL, 0, &st, &why);
    CHECK(st == GATE_STATE_ACTIVE && why == GATE_REASON_NONE);
    gateMaintain(g, t + TTL);
    INV(g);
    CHECK(g->serving != NULL && g->serving->closed == 1);
    CHECK_STATE(g, GATE_STATE_ACTIVE, GATE_REASON_NONE);
    CHECK(g->stats.revocations == 0 && g->stats.expired_eligible == 1);
    now = t + TTL;
    r = req(g, ++now, 1, 0.5, -1); /* no longer a sample of anything */
    CHECK(r.seq == 0);
    r = req(g, ++now, 0, 0.9, -1);
    CHECK_RES(r, GATE_ACTION_ACCEPT, GATE_REASON_NONE);
    /* A closed generation takes audits, but no more testing samples. */
    r = req(g, ++now, 1, 0.9, -1);
    CHECK_RES(r, GATE_ACTION_AUDIT, GATE_REASON_NONE);
    CHECK_FB(fb(g, ++now, r.seq, 1), GATE_FB_APPLIED);
    CHECK(g->serving->samples == 31 && g->serving->regions[0].n == 31);
    CHECK(g->serving->drift[0].total == 1);
    gateFree(g);

    /* (e) A training-only observation is dropped silently. */
    cfg_classifier(&c);
    g = mk(&c);
    CHECK(ctrain(g, ++now, &VA, 1) == GATE_OK);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    t = ++now;
    r = creq(g, t, 0, &VA);
    CHECK(r.seq != 0);
    gateMaintain(g, t + TTL);
    INV(g);
    CHECK(g->candidate != NULL && g->npending == 0 && g->pending_bytes == 0);
    CHECK(g->stats.expired == 1 && g->stats.expired_eligible == 0);
    CHECK(g->stats.candidates_discarded == 0);
    CHECK_FB(clearn(g, t + TTL, r.seq, 1), GATE_FB_EXPIRED);
    CHECK(g->trainer.updates == 1); /* late feedback does not train either */
    gateFree(g);

    /* (f) A labelled observation waiting behind the one that expires is not
     * itself expired by the TTL. */
    cfg_score(&c);
    g = mk(&c);
    CHECK_EQ_INT(gateFreeze(g, T0 + 1, &gen), GATE_FREEZE_OK);
    o1 = req(g, T0 + 10, 1, 0.9, -1).seq;
    o2 = req(g, T0 + 10, 1, 0.9, -1).seq;
    CHECK_FB(fb(g, T0 + 20, o2, 1), GATE_FB_APPLIED);
    gateMaintain(g, T0 + 10 + TTL);
    INV(g);
    CHECK(g->candidate == NULL && g->npending == 0);
    CHECK_EQ_INT(g->stats.expired, 1);    /* o1 only */
    CHECK_EQ_INT(g->expired_through, o1); /* o2 was consumed, not expired */
    CHECK_FB(fb(g, T0 + 10 + TTL, o1, 1), GATE_FB_EXPIRED);
    CHECK_FB(fb(g, T0 + 10 + TTL, o2, 1), GATE_FB_DUPLICATE);
    gateFree(g);
}

/* 7. ONMISSING ERROR: the same situations impute a disagreement instead. */
TEST(missing_label_error) {
    gateConfig c;
    gatePredictResult r;
    int64_t now = T0, t, first_unanswered = 0;
    uint64_t gen, o1, o2;
    gate *g;
    int i;

    /* (a) A calibration sample. */
    cfg_score(&c);
    c.on_missing = GATE_ONMISSING_ERROR;
    g = mk(&c);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    t = ++now;
    o1 = req(g, t, 1, 0.9, -1).seq;
    gateMaintain(g, t + TTL - 1);
    CHECK(g->candidate->samples == 0);
    gateMaintain(g, t + TTL);
    INV(g);
    CHECK(g->candidate != NULL && g->npending == 0);
    CHECK_STATE(g, GATE_STATE_CALIBRATING, GATE_REASON_COLD_START);
    CHECK(g->candidate->samples == 1 && g->candidate->errors == 1 && g->candidate->imputed == 1);
    CHECK(g->candidate->regions[0].n == 1 && g->candidate->regions[0].errors == 1);
    CHECK(g->candidate->regions[2].n == 1 && g->candidate->regions[2].errors == 1);
    CHECK(g->candidate->regions[3].n == 0); /* 0.9 is below threshold 1.0 */
    CHECK(g->stats.imputed == 1 && g->stats.expired == 1 && g->stats.expired_eligible == 1);
    CHECK(g->stats.candidates_discarded == 0);
    CHECK_FB(fb(g, t + TTL, o1, 1), GATE_FB_EXPIRED);
    CHECK(g->candidate->samples == 1); /* the real label came too late */

    /* The imputed error is applied in its place in the stream: before a
     * younger observation that was labelled in time. */
    now = t + TTL;
    o1 = req(g, now + 1, 1, 0.9, -1).seq;
    o2 = req(g, now + 2, 1, 0.9, -1).seq;
    CHECK_FB(fb(g, now + 3, o2, 1), GATE_FB_APPLIED);
    CHECK(g->candidate->samples == 1);
    gateMaintain(g, now + 1 + TTL);
    INV(g);
    CHECK(g->candidate->samples == 3 && g->candidate->errors == 2);
    CHECK(g->candidate->imputed == 2 && g->npending == 0);
    (void)o1;
    gateFree(g);

    /* (c) An AUDIT: one error in the drift window, serving goes on. */
    cfg_score(&c);
    c.on_missing = GATE_ONMISSING_ERROR;
    g = mk(&c);
    now = promote_everything(g, T0);
    t = ++now;
    r = req(g, t, 1, 0.9, -1);
    CHECK_RES(r, GATE_ACTION_AUDIT, GATE_REASON_NONE);
    gateMaintain(g, t + TTL);
    INV(g);
    CHECK_STATE(g, GATE_STATE_ACTIVE, GATE_REASON_NONE);
    CHECK(g->serving->drift[0].total == 1 && g->serving->drift[0].total_errors == 1);
    CHECK(g->stats.imputed == 1 && g->stats.revocations == 0);
    CHECK(g->serving->samples == 28 && g->serving->imputed == 0); /* closed: no region */

    /* (d) Enough of them trip the drift guard. 60 answered audits, then
     * unanswered ones: with the earlier one the window holds 61 audits and
     * 1 error. After 39 more unanswered it holds 100 with 40 errors, which
     * is exactly the limit 0.4 * 100; the next one is one too many. */
    now = t + TTL;
    for (i = 0; i < 60; i++) {
        r = req(g, ++now, 1, 0.9, -1);
        CHECK_FB(fb(g, now, r.seq, 1), GATE_FB_APPLIED);
    }
    for (i = 0; i < 40; i++) {
        r = req(g, ++now, 1, 0.9, -1);
        CHECK_RES(r, GATE_ACTION_AUDIT, GATE_REASON_NONE);
        if (i == 0) first_unanswered = now;
    }
    gateMaintain(g, first_unanswered + 38 + TTL); /* the first 39 of them expire */
    INV(g);
    CHECK(g->serving != NULL);
    CHECK(g->serving != NULL && g->serving->drift[0].n == 100 && g->serving->drift[0].errors == 40);
    CHECK_EQ_INT(g->npending, 1);
    gateMaintain(g, first_unanswered + 39 + TTL); /* and the 40th */
    INV(g);
    CHECK(g->serving == NULL);
    CHECK_STATE(g, GATE_STATE_REVOKED, GATE_REASON_DRIFT_DETECTED);
    CHECK_EQ_INT(g->stats.imputed, 41);
    CHECK_EQ_INT(g->stats.revocations, 1);
    CHECK(g->last_revoked_reason == GATE_REASON_DRIFT_DETECTED);
    gateFree(g);
}

/* 8. MAXPENDING. */
TEST(resource_limit_pending) {
    gateConfig c;
    gatePredictResult r;
    int64_t now = T0;
    uint64_t gen, o1, o2, fp;
    gate *g;

    /* Eligible + REVOKE: the request cannot be recorded, so the candidate it
     * was sampled for is gone. */
    cfg_score(&c);
    c.max_pending = 2;
    g = mk(&c);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    o1 = req(g, ++now, 1, 0.9, -1).seq;
    o2 = req(g, ++now, 1, 0.9, -1).seq;
    CHECK(o1 == 1 && o2 == 2 && g->npending == 2);
    r = req(g, ++now, 1, 0.9, -1);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_RESOURCE_LIMIT);
    CHECK(r.seq == 0 && r.generation == 0);
    CHECK(g->candidate == NULL);
    CHECK_STATE(g, GATE_STATE_COLD, GATE_REASON_COLD_START);
    CHECK_EQ_INT(g->stats.resource_limited, 1);
    CHECK_EQ_INT(g->stats.candidates_discarded, 1);
    CHECK_EQ_INT(g->stats.abstain_reason[GATE_REASON_RESOURCE_LIMIT], 1);
    CHECK(g->stats.observations == 2 && g->stats.eligible == 2);
    CHECK(g->next_seq == 3); /* no id was burned */
    CHECK(g->npending == 2 && g->neligible == 0); /* the two stored ones are orphans */
    /* A request that wants no observation is not limited by anything. */
    r = req(g, ++now, 0, 0.9, -1);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_COLD_START);
    CHECK_EQ_INT(g->stats.resource_limited, 1);
    CHECK_FB(fb(g, ++now, o1, 1), GATE_FB_APPLIED);
    CHECK(g->npending == 1);
    gateFree(g);

    /* Eligible + ERROR: imputed at once, ahead of the stored ones. */
    cfg_score(&c);
    c.max_pending = 2;
    c.on_missing = GATE_ONMISSING_ERROR;
    g = mk(&c);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    o1 = req(g, ++now, 1, 0.9, -1).seq;
    o2 = req(g, ++now, 1, 0.9, -1).seq;
    r = req(g, ++now, 1, 0.5, -1);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_RESOURCE_LIMIT);
    CHECK(r.seq == 0 && g->candidate != NULL);
    CHECK(g->candidate->samples == 1 && g->candidate->errors == 1 && g->candidate->imputed == 1);
    CHECK(g->candidate->regions[1].n == 1 && g->candidate->regions[2].n == 0); /* its score */
    CHECK(g->stats.imputed == 1 && g->stats.resource_limited == 1);
    CHECK(g->npending == 2 && g->neligible == 2);
    CHECK_FB(fb(g, ++now, o1, 1), GATE_FB_APPLIED);
    CHECK_FB(fb(g, ++now, o2, 1), GATE_FB_APPLIED);
    CHECK(g->candidate->samples == 3 && g->candidate->errors == 1);
    gateFree(g);

    /* A would-be AUDIT that cannot be recorded is not an audit: the reply
     * is RESOURCE_LIMIT and (REVOKE) serving is revoked. An ACCEPT needs no
     * room and is not affected by a full table. */
    cfg_score(&c);
    c.max_pending = 2;
    g = mk(&c);
    now = promote_everything(g, T0);
    CHECK_RES(req(g, ++now, 1, 0.9, -1), GATE_ACTION_AUDIT, GATE_REASON_NONE);
    CHECK_RES(req(g, ++now, 1, 0.9, -1), GATE_ACTION_AUDIT, GATE_REASON_NONE);
    CHECK_RES(req(g, ++now, 0, 0.9, -1), GATE_ACTION_ACCEPT, GATE_REASON_NONE);
    r = req(g, ++now, 1, 0.9, -1);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_RESOURCE_LIMIT);
    CHECK(r.seq == 0 && r.generation == 0 && r.label == -1);
    CHECK_STATE(g, GATE_STATE_REVOKED, GATE_REASON_FEEDBACK_INTEGRITY);
    CHECK(g->stats.audited == 2 && g->stats.would_accept == 4 && g->stats.revocations == 1);
    CHECK_EQ_INT(g->stats.abstain_reason[GATE_REASON_RESOURCE_LIMIT], 1);
    gateFree(g);

    /* Not eligible: nothing but counters. */
    cfg_classifier(&c);
    c.max_pending = 2;
    g = mk(&c);
    CHECK(ctrain(g, ++now, &VA, 1) == GATE_OK);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    creq(g, ++now, 0, &VA);
    creq(g, ++now, 0, &VB);
    fp = fingerprint(g, FP_ALL);
    r = creq(g, ++now, 0, &VA); /* creq checks that the features stay ours */
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_RESOURCE_LIMIT);
    CHECK(r.seq == 0 && g->candidate != NULL && g->npending == 2 && g->next_seq == 3);
    CHECK(g->stats.resource_limited == 1 && g->stats.requests == 3 && g->stats.abstained == 3);
    /* Undo the three counters and the clock: everything else is as before. */
    g->stats.resource_limited--;
    g->stats.requests--;
    g->stats.abstained--;
    g->stats.abstain_reason[GATE_REASON_RESOURCE_LIMIT]--;
    g->now_ms--;
    CHECK(fingerprint(g, FP_ALL) == fp);
    gateFree(g);
}

/* 8. MAXMEMORY, to the byte. */
TEST(resource_limit_memory) {
    gateConfig c;
    gatePredictResult r;
    int64_t now = T0;
    uint64_t fixed, first, genbytes;
    vec v16;
    gate *g;
    uint32_t i;

    v16.n = 16;
    for (i = 0; i < 16; i++) {
        v16.idx[i] = i;
        v16.val[i] = 0.25f;
    }
    cfg_classifier(&c);
    fixed = gateFixedBytes(&c);
    /* Room for two 16-feature observations and one 1-feature observation. */
    c.max_memory = fixed + 2 * GATE_OBS_BYTES(16) + GATE_OBS_BYTES(1);
    g = mk(&c);
    CHECK(g->fixed_bytes == fixed);

    first = creq(g, ++now, 0, &v16).seq;
    CHECK(first != 0);
    CHECK(creq(g, ++now, 0, &v16).seq != 0);
    CHECK_EQ_INT(g->pending_bytes, 2 * (sizeof(gateObs) + 16 * 8));
    r = creq(g, ++now, 0, &v16); /* 120 bytes more than what is left */
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_RESOURCE_LIMIT);
    CHECK(r.seq == 0 && g->npending == 2 && g->stats.resource_limited == 1);
    CHECK(creq(g, ++now, 0, &VA).seq != 0); /* a smaller one fits exactly */
    CHECK_EQ_INT(g->fixed_bytes + g->pending_bytes, g->cfg.max_memory);
    r = creq(g, ++now, 0, &V0); /* not even a featureless one fits now */
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_RESOURCE_LIMIT);
    CHECK_EQ_INT(g->stats.resource_limited, 2);
    CHECK_EQ_INT(g->npending, 3);

    /* Feedback frees the room again. */
    CHECK_FB(clearn(g, ++now, first, 1), GATE_FB_APPLIED);
    CHECK_EQ_INT(g->pending_bytes, GATE_OBS_BYTES(16) + GATE_OBS_BYTES(1));
    CHECK(creq(g, ++now, 0, &v16).seq != 0);
    CHECK_EQ_INT(g->fixed_bytes + g->pending_bytes, g->cfg.max_memory);

    /* No generation is alive: the two that the footprint reserves are not
     * counted as used. One generation of this gate: the struct, 17 float32
     * of frozen model, 4 regions, 1 policy entry, 1 drift window. */
    genbytes = sizeof(gateGeneration) + 17 * sizeof(float) + 4 * sizeof(gateRegion) +
               sizeof(int32_t) + sizeof(gateDriftWindow);
    CHECK_EQ_INT(gateMemoryUsage(g), fixed - 2 * genbytes + g->pending_bytes);
    gateFree(g);
}

/* 9. EXPAND: with it the policy extends as lower thresholds certify; without
 * it the policy and the regions are frozen at promotion. */
TEST(expand) {
    gateConfig c;
    gatePredictResult r;
    gateRegion snapshot[4];
    int64_t now_y = T0, now_n = T0;
    uint64_t gen;
    gate *y, *n; /* EXPAND YES / NO, fed the same stream */
    int i;

    cfg_score(&c);
    c.expand = 1;
    y = mk(&c);
    cfg_score(&c);
    n = mk(&c);

    /* Two errors at 0.1 (region 0), one at 0.5 (regions 0, 1), 28 clean
     * samples at 0.9 (regions 0, 1, 2): region 2 certifies, promotion. */
    CHECK_EQ_INT(gateFreeze(y, ++now_y, &gen), GATE_FREEZE_OK);
    CHECK_EQ_INT(gateFreeze(n, ++now_n, &gen), GATE_FREEZE_OK);
    for (i = 0; i < 2; i++) {
        sample(y, ++now_y, 0.1, -1, 0);
        sample(n, ++now_n, 0.1, -1, 0);
    }
    sample(y, ++now_y, 0.5, -1, 0);
    sample(n, ++now_n, 0.5, -1, 0);
    for (i = 0; i < 28; i++) {
        sample(y, ++now_y, 0.9, -1, 1);
        sample(n, ++now_n, 0.9, -1, 1);
    }
    CHECK(y->serving != NULL && n->serving != NULL);
    if (y->serving == NULL || n->serving == NULL) {
        gateFree(y);
        gateFree(n);
        return;
    }
    CHECK(y->serving->policy[0] == 2 && n->serving->policy[0] == 2);
    CHECK(y->serving->closed == 0 && n->serving->closed == 1);
    CHECK(y->serving->policy_updates == 1 && n->serving->policy_updates == 1);
    CHECK_NEAR(gateGenerationCoverage(y, y->serving), 28.0 / 31.0, 1e-15);
    memcpy(snapshot, n->serving->regions, sizeof(snapshot));

    /* Audited requests at 0.5, all correct. EXPAND NO: below the threshold,
     * nobody wants them. EXPAND YES: they are testing samples for regions 0
     * and 1. Region 1 saw [error, 28 correct] so far and certifies on its
     * 40th sample, i.e. after 11 more. */
    for (i = 1; i <= 10; i++) {
        r = req(n, ++now_n, 1, 0.5, -1);
        CHECK(r.seq == 0);
        r = req(y, ++now_y, 1, 0.5, -1);
        CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_BELOW_THRESHOLD);
        CHECK(r.seq != 0);
        CHECK(gateFindObservation(y, r.seq)->flags == (GATE_OBS_AUDIT | GATE_OBS_SERV));
        CHECK_FB(fb(y, now_y, r.seq, 1), GATE_FB_APPLIED);
        CHECK(y->serving->policy[0] == 2);
    }
    CHECK(y->serving->regions[1].n == 39 && y->serving->samples == 41);
    sample(y, ++now_y, 0.5, -1, 1);
    CHECK_EQ_INT(y->serving->policy[0], 1);
    CHECK_EQ_INT(y->serving->policy_updates, 2);
    CHECK_EQ_INT(y->serving->regions[1].certified_at, 40);
    CHECK_NEAR(gateGenerationCoverage(y, y->serving), 40.0 / 42.0, 1e-15);
    CHECK_EQ_INT(y->stats.promotions, 1); /* an extension, not a new generation */
    CHECK_NEAR(gateBudgetSpentNow(y), 0.05, 1e-15); /* and no new budget */

    /* 0.5 is inside the policy now. */
    r = req(y, ++now_y, 0, 0.5, -1);
    CHECK_RES(r, GATE_ACTION_ACCEPT, GATE_REASON_NONE);
    r = req(n, ++now_n, 0, 0.5, -1);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_BELOW_THRESHOLD);

    /* Region 0 saw [3 errors, 28 + 11 correct] and certifies on its 63rd
     * sample: 21 more. Audits at 0.5 are would-accepts now, so each also
     * lands in the drift window. */
    for (i = 1; i <= 20; i++) {
        r = req(y, ++now_y, 1, 0.5, -1);
        CHECK_RES(r, GATE_ACTION_AUDIT, GATE_REASON_NONE);
        CHECK_FB(fb(y, now_y, r.seq, 1), GATE_FB_APPLIED);
        CHECK(y->serving->policy[0] == 1);
    }
    CHECK(y->serving->regions[0].n == 62 && y->serving->drift[0].total == 20);
    r = req(y, ++now_y, 1, 0.5, -1);
    CHECK_FB(fb(y, now_y, r.seq, 1), GATE_FB_APPLIED);
    CHECK_EQ_INT(y->serving->policy[0], 0);
    CHECK_EQ_INT(y->serving->policy_updates, 3);
    CHECK_EQ_INT(y->serving->regions[0].certified_at, 63);
    CHECK(gateGenerationCoverage(y, y->serving) == 1.0);
    r = req(y, ++now_y, 0, 0.0, -1);
    CHECK_RES(r, GATE_ACTION_ACCEPT, GATE_REASON_NONE);

    /* EXPAND NO never moved. */
    CHECK(n->serving->policy[0] == 2 && n->serving->policy_updates == 1);
    CHECK(n->serving->samples == 31);
    CHECK(memcmp(snapshot, n->serving->regions, sizeof(snapshot)) == 0);
    gateFree(y);
    gateFree(n);
}

/* 9. EXPAND in class mode: a second class gets its own threshold later. */
TEST(expand_adds_classes) {
    gateConfig c;
    gatePredictResult r;
    int64_t now = T0;
    uint64_t gen;
    gate *g;
    int i;

    cfg_classes(&c);
    c.expand = 1;
    g = mk(&c);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    for (i = 0; i < 32; i++) sample(g, ++now, 1.0, 0, 1); /* class a: 32 at alpha 0.2 */
    CHECK(g->serving != NULL);
    if (g->serving == NULL) {
        gateFree(g);
        return;
    }
    CHECK(g->serving->policy[0] == 0 && g->serving->policy[1] == -1 && g->serving->policy[2] == -1);
    r = req(g, ++now, 0, 1.0, 1);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_CLASS_DEFERRED);

    /* Class b keeps being tested after the promotion: 68 at alpha 0.1. */
    for (i = 1; i <= 67; i++) {
        r = req(g, ++now, 1, 1.0, 1);
        CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_CLASS_DEFERRED);
        CHECK(r.seq != 0);
        CHECK_FB(fb(g, now, r.seq, 1), GATE_FB_APPLIED);
    }
    CHECK(g->serving->policy[1] == -1 && g->serving->policy_updates == 1);
    sample(g, ++now, 1.0, 1, 1);
    CHECK_EQ_INT(g->serving->policy[1], 0);
    CHECK_EQ_INT(g->serving->policy_updates, 2);
    r = req(g, ++now, 0, 1.0, 1);
    CHECK_RES(r, GATE_ACTION_ACCEPT, GATE_REASON_NONE);
    CHECK(r.label == 1 && r.risk_target == 0.1);
    /* The ALWAYSDEFER class is sampled (it is traffic) but never tested. */
    r = req(g, ++now, 1, 1.0, 2);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_CLASS_DEFERRED);
    CHECK_FB(fb(g, now, r.seq, 1), GATE_FB_APPLIED);
    CHECK(g->serving->samples == 32 + 68 + 1 && g->serving->regions[2 * 4].n == 0);
    CHECK(g->serving->policy[2] == -1);
    gateFree(g);
}

/* 10. Class-conditional gates. */
TEST(class_conditional) {
    gateConfig c;
    gatePredictResult r;
    gateRegion row_b[4];
    gateEval ev;
    int64_t now = T0;
    uint64_t gen, fp;
    uint32_t t;
    gate *g;
    int i;

    cfg_classes(&c);
    g = mk(&c);
    CHECK_EQ_INT(g->nrows, 3);
    CHECK(g->bets[0].alpha == 0.2 && g->bets[1].alpha == 0.1);
    CHECK(gateRowAlpha(g, 0) == 0.2 && gateRowAlpha(g, 1) == 0.1 && gateRowAlpha(g, 2) == 0.2);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    /* Two tested rows of four thresholds: c is ALWAYSDEFER and costs no
     * budget. */
    CHECK_EQ_INT(g->candidate->regions_tested, 8);
    CHECK_NEAR(g->candidate->delta_region, 0.05 / 8, 1e-15);
    CHECK_NEAR(g->candidate->log_threshold, 5.075173815233827, 1e-12); /* ln 160 */

    /* 67 clean samples of class b: one short of its 68. */
    for (i = 0; i < 67; i++) sample(g, ++now, 1.0, 1, 1);
    CHECK(g->candidate != NULL && g->candidate->samples == 67);
    for (t = 0; t < 4; t++) {
        CHECK(g->candidate->regions[0 * 4 + t].n == 0); /* row a */
        CHECK(g->candidate->regions[1 * 4 + t].n == 67); /* row b */
        CHECK(g->candidate->regions[2 * 4 + t].n == 0); /* row c */
    }
    memcpy(row_b, g->candidate->regions + 4, sizeof(row_b));

    /* The ALWAYSDEFER class counts as traffic and in no region. */
    for (i = 0; i < 5; i++) sample(g, ++now, 1.0, 2, 1);
    CHECK(g->candidate->samples == 72);
    for (t = 0; t < 4; t++) CHECK(g->candidate->regions[2 * 4 + t].n == 0);

    /* Class a at score 0.5: regions "a, >= 0" and "a, >= 1/3". 31 are not
     * enough at alpha 0.2 with 8 regions, the 32nd is. */
    for (i = 0; i < 31; i++) sample(g, ++now, 0.5, 0, 1);
    CHECK(g->candidate != NULL && g->serving == NULL);
    CHECK(g->candidate->regions[0].n == 31 && g->candidate->regions[1].n == 31);
    CHECK(g->candidate->regions[2].n == 0);
    CHECK(memcmp(row_b, g->candidate->regions + 4, sizeof(row_b)) == 0); /* b never moved */
    sample(g, ++now, 0.5, 0, 1);
    CHECK(g->serving != NULL && g->candidate == NULL);
    if (g->serving == NULL) {
        gateFree(g);
        return;
    }
    CHECK_EQ_INT(g->serving->samples, 104);
    CHECK(g->serving->policy[0] == 0 && g->serving->policy[1] == -1 && g->serving->policy[2] == -1);
    CHECK_NEAR(gateGenerationCoverage(g, g->serving), 32.0 / 104.0, 1e-15);

    /* a is accepted at its own alpha; b has no certified region; c never. */
    r = req(g, ++now, 0, 0.0, 0);
    CHECK_RES(r, GATE_ACTION_ACCEPT, GATE_REASON_NONE);
    CHECK(r.label == 0 && r.risk_target == 0.2 && r.score == 0.0);
    r = req(g, ++now, 0, 1.0, 1);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_CLASS_DEFERRED);
    CHECK(r.label == -1 && r.generation == 1);
    r = req(g, ++now, 0, 1.0, 2);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_CLASS_DEFERRED);
    r = req(g, ++now, 1, 1.0, 1); /* EXPAND NO: b is not being tested any more */
    CHECK(r.seq == 0);
    CHECK_EQ_INT(g->stats.abstain_reason[GATE_REASON_CLASS_DEFERRED], 3);

    /* Audits go to the drift window of the predicted class. */
    r = req(g, ++now, 1, 0.3, 0);
    CHECK_RES(r, GATE_ACTION_AUDIT, GATE_REASON_NONE);
    CHECK(r.label == 0);
    CHECK_FB(fb(g, now, r.seq, 0), GATE_FB_APPLIED);
    CHECK(g->serving->drift[0].total == 1 && g->serving->drift[0].total_errors == 1);
    CHECK(g->serving->drift[1].total == 0 && g->serving->drift[2].total == 0);

    /* A class index that is not a class is refused before anything moves. */
    gateMaintain(g, now);
    ev.has_serving = 1;
    ev.has_candidate = 0;
    ev.serving.score = 0.9;
    ev.serving.label = 3;
    fp = fingerprint(g, FP_ALL);
    CHECK(gatePredict(g, now, 0, &ev, NULL, &r) == GATE_ERR);
    ev.serving.label = -1;
    CHECK(gatePredict(g, now, 0, &ev, NULL, &r) == GATE_ERR);
    CHECK(gateEvaluate(g, NULL, 0.9, 3, &ev) == GATE_ERR);
    CHECK(fingerprint(g, FP_ALL) == fp);
    gateFree(g);

    /* The stricter class certifies first if it gets its evidence first, and
     * is then accepted at its own alpha. */
    cfg_classes(&c);
    g = mk(&c);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    for (i = 0; i < 67; i++) sample(g, ++now, 1.0, 1, 1);
    CHECK(g->serving == NULL);
    sample(g, ++now, 1.0, 1, 1);
    CHECK(g->serving != NULL && g->serving->policy[1] == 0 && g->serving->policy[0] == -1);
    r = req(g, ++now, 0, 1.0, 1);
    CHECK_RES(r, GATE_ACTION_ACCEPT, GATE_REASON_NONE);
    CHECK(r.label == 1 && r.risk_target == 0.1);
    r = req(g, ++now, 0, 1.0, 0);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_CLASS_DEFERRED);
    gateFree(g);
}

/* 11. The drift guard. */
static void audits(gate *g, int64_t *now, int n, int correct) {
    int i;

    for (i = 0; i < n; i++) {
        gatePredictResult r = req(g, ++*now, 1, 0.9, -1);

        CHECK_RES(r, GATE_ACTION_AUDIT, GATE_REASON_NONE);
        CHECK_FB(fb(g, *now, r.seq, correct), GATE_FB_APPLIED);
    }
}

TEST(drift_guard) {
    gateConfig c;
    gatePredictResult r;
    int64_t now, tripped_at;
    uint64_t gen;
    gate *g;
    int i;

    /* The alarm needs GATE_DRIFT_MIN audits: 50 errors in 99 audits do not
     * trip it, the 100th audit does, although it is a correct one. */
    cfg_score(&c);
    g = mk(&c);
    now = promote_everything(g, T0);
    audits(g, &now, 50, 0);
    audits(g, &now, 49, 1);
    CHECK(g->serving != NULL && g->serving->drift[0].n == 99 && g->serving->drift[0].errors == 50);
    audits(g, &now, 1, 1);
    CHECK(g->serving == NULL);
    CHECK_STATE(g, GATE_STATE_REVOKED, GATE_REASON_DRIFT_DETECTED);
    gateFree(g);

    /* The limit is alpha + alpha = 0.4, exclusive: 40 errors in 100 audits
     * are tolerated, 41 in 101 are not. */
    cfg_score(&c);
    g = mk(&c);
    now = promote_everything(g, T0);
    audits(g, &now, 60, 1);
    audits(g, &now, 40, 0);
    CHECK(g->serving != NULL && g->serving->drift[0].n == 100 && g->serving->drift[0].errors == 40);
    CHECK_STATE(g, GATE_STATE_ACTIVE, GATE_REASON_NONE);
    audits(g, &now, 1, 0);
    tripped_at = now;
    CHECK(g->serving == NULL);
    CHECK_STATE(g, GATE_STATE_REVOKED, GATE_REASON_DRIFT_DETECTED);
    CHECK(g->stats.revocations == 1 && g->revoked_reason == GATE_REASON_DRIFT_DETECTED);
    CHECK(g->last_revoked_reason == GATE_REASON_DRIFT_DETECTED);
    CHECK(g->last_revoked_gen == 1 && g->last_revoked_ms == tripped_at);
    r = req(g, ++now, 0, 0.9, -1);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_DRIFT_DETECTED);
    CHECK(r.generation == 0 && r.seq == 0);
    CHECK(gateAuditProbability(g) == 0.5);

    /* Recalibration needs a new generation: CALIBRATING, with the reason of
     * the revocation still reported, then ACTIVE on the 34th clean sample of
     * generation 2. The history of the revocation is kept. */
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    CHECK_STATE(g, GATE_STATE_CALIBRATING, GATE_REASON_DRIFT_DETECTED);
    r = req(g, ++now, 0, 0.9, -1);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_DRIFT_DETECTED);
    for (i = 0; i < 33; i++) sample(g, ++now, 1.0, -1, 1);
    CHECK_STATE(g, GATE_STATE_CALIBRATING, GATE_REASON_DRIFT_DETECTED);
    sample(g, ++now, 1.0, -1, 1);
    CHECK_STATE(g, GATE_STATE_ACTIVE, GATE_REASON_NONE);
    CHECK(g->serving != NULL && g->serving->gen == 2 && g->serving->samples == 34);
    CHECK(g->revoked_reason == GATE_REASON_NONE);
    CHECK(g->last_revoked_reason == GATE_REASON_DRIFT_DETECTED);
    CHECK(g->last_revoked_gen == 1 && g->last_revoked_ms == tripped_at);
    CHECK(g->serving->drift[0].total == 0); /* a fresh window */
    gateFree(g);

    /* The window is the last 256 audits: 256 correct ones slide out as
     * errors come in. 102 errors of 256 are within 0.4 * 256 = 102.4, the
     * 103rd is not. */
    cfg_score(&c);
    g = mk(&c);
    now = promote_everything(g, T0);
    audits(g, &now, 256, 1);
    audits(g, &now, 102, 0);
    CHECK(g->serving != NULL && g->serving->drift[0].n == 256);
    CHECK(g->serving != NULL && g->serving->drift[0].errors == 102);
    CHECK(g->serving != NULL && g->serving->drift[0].total == 358);
    audits(g, &now, 1, 0);
    CHECK(g->serving == NULL && g->revoked_reason == GATE_REASON_DRIFT_DETECTED);
    gateFree(g);

    /* DRIFTMARGIN 0.05: the limit is 0.25. 25 of 100 pass, 26 of 101 trip. */
    cfg_score(&c);
    c.drift_margin = 0.05;
    g = mk(&c);
    now = promote_everything(g, T0);
    audits(g, &now, 75, 1);
    audits(g, &now, 25, 0);
    CHECK(g->serving != NULL);
    audits(g, &now, 1, 0);
    CHECK(g->serving == NULL && g->revoked_reason == GATE_REASON_DRIFT_DETECTED);
    gateFree(g);

    /* With a candidate already calibrating in the background, the gate goes
     * from ACTIVE to CALIBRATING. The tripping audit (score 1.0: in all four
     * regions) was a sample of the candidate too, an error and its first, so
     * generation 2 certifies on its 46th sample. */
    cfg_score(&c);
    g = mk(&c);
    now = promote_everything(g, T0);
    audits(g, &now, 60, 1);
    audits(g, &now, 40, 0);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    CHECK_STATE(g, GATE_STATE_ACTIVE, GATE_REASON_NONE);
    r = req(g, ++now, 1, 1.0, -1);
    CHECK_RES(r, GATE_ACTION_AUDIT, GATE_REASON_NONE);
    CHECK_FB(fb(g, now, r.seq, 0), GATE_FB_APPLIED);
    CHECK(g->serving == NULL && g->candidate != NULL);
    CHECK_STATE(g, GATE_STATE_CALIBRATING, GATE_REASON_DRIFT_DETECTED);
    CHECK(g->candidate->samples == 1 && g->candidate->errors == 1);
    for (i = 0; i < 44; i++) sample(g, ++now, 1.0, -1, 1);
    CHECK_STATE(g, GATE_STATE_CALIBRATING, GATE_REASON_DRIFT_DETECTED);
    sample(g, ++now, 1.0, -1, 1);
    CHECK_STATE(g, GATE_STATE_ACTIVE, GATE_REASON_NONE);
    CHECK(g->serving != NULL && g->serving->gen == 2 && g->serving->samples == 46);
    CHECK(g->stats.promotions == 2 && g->stats.revocations == 1);
    CHECK(g->last_revoked_reason == GATE_REASON_DRIFT_DETECTED && g->last_revoked_gen == 1);
    gateFree(g);
}

/* 12. Certificate expiry. */
TEST(certificate_expiry) {
    gateConfig c;
    gatePredictResult r;
    gateStateId st;
    gateReason why;
    gateEval stale;
    int64_t now, promoted, t;
    gate *g;

    cfg_score(&c);
    c.cert_ttl_ms = 5000;
    g = mk(&c);
    promoted = promote_everything(g, T0);
    CHECK_EQ_INT(g->serving->expires_ms, promoted + 5000);

    r = req(g, promoted + 4999, 0, 0.9, -1);
    CHECK_RES(r, GATE_ACTION_ACCEPT, GATE_REASON_NONE);
    CHECK(gateEvaluate(g, NULL, 0.9, -1, &stale) == GATE_OK);
    /* GINFO reports the expiry before any command has acted on it. */
    gatePreview(g, promoted + 4999, 0, &st, &why);
    CHECK(st == GATE_STATE_ACTIVE && why == GATE_REASON_NONE);
    gatePreview(g, promoted + 5000, 0, &st, &why);
    CHECK(st == GATE_STATE_REVOKED && why == GATE_REASON_CERT_EXPIRED);
    CHECK(gateState(g) == GATE_STATE_ACTIVE && g->serving != NULL);
    CHECK(gateAuditProbability(g) == 0.25);
    /* A fence that has not been applied yet, and a pause, show through. */
    gatePreview(g, promoted + 4999, 1, &st, &why);
    CHECK(st == GATE_STATE_REVOKED && why == GATE_REASON_RECOVERY_REQUIRED);
    g->paused = 1;
    gatePreview(g, promoted + 5000, 0, &st, &why);
    CHECK(st == GATE_STATE_PAUSED && why == GATE_REASON_PAUSED);
    gatePreview(g, promoted + 5000, 1, &st, &why);
    CHECK(st == GATE_STATE_PAUSED && why == GATE_REASON_RECOVERY_REQUIRED);
    g->paused = 0;

    /* The first command at the expiry time revokes before it decides. */
    CHECK(gatePredict(g, promoted + 5000, 0, &stale, NULL, &r) == GATE_OK);
    INV(g);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_CERT_EXPIRED);
    CHECK(r.generation == 0);
    CHECK_STATE(g, GATE_STATE_REVOKED, GATE_REASON_CERT_EXPIRED);
    CHECK(g->stats.revocations == 1 && g->last_revoked_reason == GATE_REASON_CERT_EXPIRED);
    CHECK(g->last_revoked_ms == promoted + 5000 && g->last_revoked_gen == 1);
    CHECK(gateAuditProbability(g) == 0.5);
    gateFree(g);

    /* An unanswered audit of a certificate that expired anyway: the reason
     * is the certificate, in the preview and in fact. */
    cfg_score(&c);
    c.cert_ttl_ms = 5000;
    g = mk(&c);
    promoted = promote_everything(g, T0);
    t = promoted + 4500;
    r = req(g, t, 1, 0.9, -1);
    CHECK_RES(r, GATE_ACTION_AUDIT, GATE_REASON_NONE);
    gatePreview(g, promoted + 6000, 0, &st, &why);
    CHECK(st == GATE_STATE_REVOKED && why == GATE_REASON_CERT_EXPIRED);
    gateMaintain(g, promoted + 6000);
    INV(g);
    CHECK_STATE(g, GATE_STATE_REVOKED, GATE_REASON_CERT_EXPIRED);
    CHECK(g->stats.revocations == 1 && g->npending == 0);
    CHECK(g->stats.expired == 1 && g->stats.expired_eligible == 1);
    now = promoted + 6000;
    r = req(g, ++now, 0, 0.9, -1);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_CERT_EXPIRED);
    gateFree(g);
}

/* 13. A candidate calibrating in the background of an ACTIVE gate. */
TEST(background_candidate) {
    gateConfig c;
    gatePredictResult r;
    int64_t now = T0, t;
    uint64_t gen, promoting, late, later;
    const gateObs *o;
    gate *g;
    int i;

    cfg_score(&c);
    g = mk(&c);
    now = promote_at_two_thirds(g, now); /* generation 1: "score >= 2/3" */
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    CHECK(gen == 2);
    CHECK_STATE(g, GATE_STATE_ACTIVE, GATE_REASON_NONE);

    /* One observation, two generations: the candidate's regions and the
     * serving generation's drift window. */
    r = req(g, ++now, 1, 0.9, -1);
    CHECK_RES(r, GATE_ACTION_AUDIT, GATE_REASON_NONE);
    CHECK(r.generation == 1);
    CHECK_FB(fb(g, now, r.seq, 1), GATE_FB_APPLIED);
    CHECK(g->candidate->samples == 1 && g->candidate->regions[2].n == 1);
    CHECK(g->serving->drift[0].total == 1 && g->serving->samples == 31);
    /* Below the serving threshold: a sample of the candidate only. Its
     * evidence does not depend on what the serving policy accepts. */
    r = req(g, ++now, 1, 0.5, -1);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_BELOW_THRESHOLD);
    CHECK_FB(fb(g, now, r.seq, 1), GATE_FB_APPLIED);
    CHECK(g->candidate->samples == 2 && g->candidate->regions[0].n == 2);
    CHECK(g->candidate->regions[2].n == 1 && g->serving->drift[0].total == 1);
    /* Unsampled traffic is served as before. */
    r = req(g, ++now, 0, 0.9, -1);
    CHECK_RES(r, GATE_ACTION_ACCEPT, GATE_REASON_NONE);
    CHECK(r.generation == 1);

    /* Generation 2 needs 34 clean samples in a region; regions 0 and 1 have
     * 2. 31 more audits leave them at 33. */
    for (i = 0; i < 31; i++) {
        r = req(g, ++now, 1, 0.9, -1);
        CHECK_FB(fb(g, now, r.seq, 1), GATE_FB_APPLIED);
    }
    CHECK(g->candidate != NULL && g->candidate->regions[0].n == 33);
    CHECK(g->serving->gen == 1 && g->serving->drift[0].total == 32);

    /* The 34th is predicted, then two more audits, then it is labelled. */
    promoting = req(g, ++now, 1, 0.9, -1).seq;
    late = req(g, ++now, 1, 0.9, -1).seq;
    t = ++now;
    later = req(g, t, 1, 0.9, -1).seq;
    CHECK_FB(fb(g, ++now, promoting, 1), GATE_FB_APPLIED);
    CHECK_STATE(g, GATE_STATE_ACTIVE, GATE_REASON_NONE);
    CHECK(g->candidate == NULL && g->serving != NULL && g->serving->gen == 2);
    CHECK(g->serving->samples == 34 && g->serving->policy[0] == 0);
    CHECK(g->stats.promotions == 2);
    CHECK(g->stats.revocations == 0 && g->last_revoked_gen == 0); /* replaced, not revoked */
    CHECK(g->serving->drift[0].total == 0);
    r = req(g, ++now, 0, 0.1, -1); /* inside the new policy only */
    CHECK_RES(r, GATE_ACTION_ACCEPT, GATE_REASON_NONE);
    CHECK(r.generation == 2);

    /* 'late' audited generation 1, which is gone, and sampled generation 2,
     * which stopped testing at its promotion: an error on it is nobody's. */
    o = gateFindObservation(g, late);
    CHECK(o != NULL && o->serv_gen == 1 && o->cand_gen == 2);
    CHECK_FB(fb(g, ++now, late, 0), GATE_FB_APPLIED);
    CHECK(g->serving->gen == 2 && g->serving->samples == 34 && g->serving->errors == 0);
    CHECK(g->serving->drift[0].total == 0);
    /* And letting 'later' expire is not a feedback failure of generation 2. */
    gateMaintain(g, t + TTL);
    INV(g);
    CHECK(g->serving != NULL && g->serving->gen == 2 && g->stats.revocations == 0);
    CHECK(g->npending == 0 && g->stats.expired == 1);
    (void)later;
    gateFree(g);
}

/* 14. GRESET scopes. */
TEST(resets) {
    static const gateCounters zero;
    gateConfig c;
    gatePredictResult r;
    int64_t now = T0;
    uint64_t gen, head, queued, pending, fp;
    float w[16];
    gate *g;

    cfg_score(&c);
    c.cert_ttl_ms = 50000;
    g = mk(&c);
    now = promote_at_two_thirds(g, now);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    head = req(g, ++now, 1, 0.9, -1).seq;
    queued = req(g, ++now, 1, 0.5, -1).seq;
    gatePause(g, ++now);

    /* CANDIDATE: the candidate only. */
    gateReset(g, ++now, GATE_RESET_CANDIDATE);
    INV(g);
    CHECK(g->candidate == NULL && g->serving != NULL && g->serving->gen == 1);
    CHECK(g->serving->samples == 31 && g->serving->policy[0] == 2);
    CHECK(g->stats.candidates_discarded == 1 && g->generations == 2 && g->paused == 1);
    CHECK(g->npending == 2);
    gateReset(g, ++now, GATE_RESET_CANDIDATE); /* nothing to cancel */
    CHECK(g->stats.candidates_discarded == 1);

    /* CALIBRATION: serving too. Not a revocation; observations stay. */
    gateReset(g, ++now, GATE_RESET_CALIBRATION);
    INV(g);
    CHECK(g->serving == NULL && g->candidate == NULL);
    CHECK(g->stats.revocations == 0 && g->last_revoked_gen == 0);
    CHECK(g->revoked_reason == GATE_REASON_NONE && g->generations == 2);
    CHECK(g->paused == 1 && gateState(g) == GATE_STATE_PAUSED);
    CHECK(g->npending == 2 && g->neligible == 0);
    gateResume(g, ++now);
    CHECK_STATE(g, GATE_STATE_COLD, GATE_REASON_COLD_START);
    CHECK_FB(fb(g, ++now, head, 1), GATE_FB_APPLIED);
    CHECK(g->npending == 1);

    /* ALL: pending observations and counters as well; never the ledger. */
    gatePause(g, ++now);
    gateReset(g, ++now, GATE_RESET_ALL);
    INV(g);
    CHECK(g->npending == 0 && g->pending_bytes == 0 && g->invalid_below == g->next_seq);
    CHECK(memcmp(&g->stats, &zero, sizeof(zero)) == 0);
    CHECK(g->generations == 2 && g->lineage == LINEAGE && g->epoch == EPOCH && g->paused == 1);
    CHECK_FB(fb(g, now, queued, 1), GATE_FB_INVALIDATED);
    gateResume(g, ++now);

    /* A revoked gate becomes COLD by CALIBRATION; the history of the
     * revocation is erased only by ALL. */
    now = promote_everything(g, now);
    gateMaintain(g, g->serving->expires_ms);
    now = g->now_ms;
    CHECK_STATE(g, GATE_STATE_REVOKED, GATE_REASON_CERT_EXPIRED);
    gateReset(g, ++now, GATE_RESET_CALIBRATION);
    INV(g);
    CHECK_STATE(g, GATE_STATE_COLD, GATE_REASON_COLD_START);
    CHECK(g->last_revoked_reason == GATE_REASON_CERT_EXPIRED && g->last_revoked_gen == 3);
    CHECK(g->stats.revocations == 1);
    gateReset(g, ++now, GATE_RESET_ALL);
    INV(g);
    CHECK(g->last_revoked_reason == GATE_REASON_NONE && g->last_revoked_gen == 0);
    CHECK(g->last_revoked_ms == 0 && g->generations == 3);

    /* Not a scope: nothing happens. */
    fp = fingerprint(g, FP_ALL);
    gateReset(g, now, (gateResetScope)42);
    CHECK(fingerprint(g, FP_ALL) == fp);
    gateFree(g);

    /* The trainer: kept by CALIBRATION, zeroed by MODEL. */
    cfg_classifier(&c);
    g = mk(&c);
    now = T0;
    train_ab(g, now);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    r = creq(g, ++now, 0, &VA);
    pending = r.seq;
    memcpy(w, g->trainer.w, sizeof(w));
    gateReset(g, ++now, GATE_RESET_CALIBRATION);
    INV(g);
    CHECK(g->candidate == NULL && g->trainer.updates == 800);
    CHECK(memcmp(w, g->trainer.w, sizeof(w)) == 0);
    gateReset(g, ++now, GATE_RESET_MODEL);
    INV(g);
    memset(w, 0, sizeof(w));
    CHECK(g->trainer.updates == 0 && g->trainer.b[0] == 0.0f);
    CHECK(memcmp(w, g->trainer.w, sizeof(w)) == 0);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_ERR_UNTRAINED);
    CHECK(g->generations == 1 && g->npending == 1); /* MODEL keeps the observations */
    CHECK_FB(clearn(g, ++now, pending, 1), GATE_FB_APPLIED);
    CHECK(g->trainer.updates == 1);
    gateFree(g);
}

/* 14. GCONFIG SET AUDIT / TEACHER / SCHEMAVERSION. */
TEST(config_changes) {
    gateConfig c;
    gatePredictResult r;
    char longname[GATE_MAX_VERSION_BYTES + 2];
    int64_t now = T0;
    uint64_t gen, fp, audit_seq, pending;
    float zeros[16];
    gate *g;

    /* AUDIT. */
    cfg_score(&c);
    g = mk(&c);
    now = promote_at_two_thirds(g, now);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    audit_seq = req(g, ++now, 1, 0.9, -1).seq;
    fp = fingerprint(g, FP_ALL);
    gateSetAudit(g, now, 0.25); /* the current value */
    gateSetAudit(g, now, 0.0);  /* not probabilities: ignored */
    gateSetAudit(g, now, 1.5);
    gateSetAudit(g, now, -0.25);
    gateSetAudit(g, now, NAN);
    CHECK(fingerprint(g, FP_ALL) == fp);
    INV(g);
    gateSetAudit(g, ++now, 0.75);
    INV(g);
    CHECK(g->cfg.audit == 0.75 && g->cfg.cold_audit == 0.5);
    CHECK(g->candidate == NULL && g->serving == NULL);
    CHECK_STATE(g, GATE_STATE_REVOKED, GATE_REASON_CONFIG_CHANGED);
    CHECK(g->stats.candidates_discarded == 1 && g->stats.revocations == 1);
    CHECK(g->last_revoked_reason == GATE_REASON_CONFIG_CHANGED && g->last_revoked_gen == 1);
    CHECK(g->generations == 2);
    CHECK(g->npending == 1 && g->neligible == 0); /* observations are kept, as orphans */
    CHECK_FB(fb(g, now, audit_seq, 1), GATE_FB_APPLIED);
    r = req(g, ++now, 0, 0.9, -1);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_CONFIG_CHANGED);
    gateSetAudit(g, ++now, 1.0); /* the upper bound is a valid probability */
    CHECK(g->cfg.audit == 1.0);
    INV(g);
    gateFree(g);

    /* With a candidate only there is nothing to revoke. */
    cfg_score(&c);
    g = mk(&c);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    gateSetAudit(g, ++now, 0.9);
    INV(g);
    CHECK_STATE(g, GATE_STATE_COLD, GATE_REASON_COLD_START);
    CHECK(g->stats.candidates_discarded == 1 && g->stats.revocations == 0);
    gateFree(g);

    /* TEACHER. */
    cfg_classifier(&c);
    g = mk(&c);
    now = classifier_promote(g, T0);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    pending = creq(g, ++now, 0, &VC).seq;
    fp = fingerprint(g, FP_ALL);
    CHECK(gateSetTeacher(g, now, "teacher-v1") == GATE_OK); /* the current value */
    CHECK(gateSetSchemaVersion(g, now, "schema-v1") == GATE_OK);
    /* Not version strings: refused, nothing changes. */
    memset(longname, 'x', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    CHECK(gateSetTeacher(g, now, longname) == GATE_ERR); /* 129 bytes */
    CHECK(gateSetTeacher(g, now, "") == GATE_ERR);
    CHECK(gateSetTeacher(g, now, "two\nlines") == GATE_ERR);
    CHECK(gateSetTeacher(g, now, NULL) == GATE_ERR);
    CHECK(gateSetSchemaVersion(g, now, "\xff\xfe") == GATE_ERR);
    CHECK(fingerprint(g, FP_ALL) == fp);
    INV(g);

    CHECK(gateSetTeacher(g, ++now, "teacher-v2") == GATE_OK);
    INV(g);
    CHECK_EQ_STR(g->cfg.teacher, "teacher-v2");
    CHECK_EQ_STR(g->cfg.schema_version, "schema-v1");
    CHECK(g->candidate == NULL && g->serving == NULL);
    CHECK_STATE(g, GATE_STATE_REVOKED, GATE_REASON_TEACHER_CHANGED);
    CHECK(g->stats.candidates_discarded == 1 && g->stats.revocations == 1);
    CHECK(g->last_revoked_reason == GATE_REASON_TEACHER_CHANGED);
    memset(zeros, 0, sizeof(zeros));
    CHECK(g->trainer.updates == 0 && memcmp(zeros, g->trainer.w, sizeof(zeros)) == 0);
    CHECK(g->npending == 0 && g->pending_bytes == 0 && g->invalid_below == g->next_seq);
    CHECK(g->generations == 2); /* the budget is not replenished */
    CHECK_FB(clearn(g, now, pending, 1), GATE_FB_INVALIDATED);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_ERR_UNTRAINED);
    gateFree(g);

    /* SCHEMAVERSION. */
    cfg_score(&c);
    g = mk(&c);
    now = promote_at_two_thirds(g, T0);
    audit_seq = req(g, ++now, 1, 0.9, -1).seq;
    CHECK(gateSetSchemaVersion(g, ++now, "schema-v2-with-a-longer-name") == GATE_OK);
    INV(g);
    CHECK_EQ_STR(g->cfg.schema_version, "schema-v2-with-a-longer-name");
    CHECK_STATE(g, GATE_STATE_REVOKED, GATE_REASON_SCHEMA_CHANGED);
    CHECK(g->last_revoked_reason == GATE_REASON_SCHEMA_CHANGED && g->npending == 0);
    CHECK_FB(fb(g, now, audit_seq, 1), GATE_FB_INVALIDATED);
    CHECK(g->fixed_bytes == gateFixedBytes(&g->cfg));
    gateFree(g);

    /* The version strings are part of the footprint MAXMEMORY was validated
     * against: with no byte to spare, a longer name does not fit. */
    cfg_score(&c);
    c.max_memory = gateFixedBytes(&c);
    g = mk(&c);
    fp = fingerprint(g, FP_ALL);
    CHECK(gateSetTeacher(g, T0, "teacher-v1x") == GATE_ERR);
    CHECK(fingerprint(g, FP_ALL) == fp);
    CHECK(gateSetTeacher(g, T0, "teacher-v2") == GATE_OK); /* same length */
    CHECK(gateSetTeacher(g, T0, "t") == GATE_OK);          /* shorter */
    CHECK(g->fixed_bytes == gateFixedBytes(&g->cfg) && g->fixed_bytes < g->cfg.max_memory);
    CHECK(gateSetSchemaVersion(g, T0, "schema-v1-plus-9ch") == GATE_OK); /* the 9 bytes saved */
    CHECK(g->fixed_bytes == g->cfg.max_memory);
    CHECK(gateSetTeacher(g, T0, "tt") == GATE_ERR);
    INV(g);
    gateFree(g);
}

/* 14. gateFence() / gateRecover(). */
TEST(fence_and_recover) {
    gateConfig c;
    gateFeedback f = {GATE_FEEDBACK_LABEL, 1, NULL, 0};
    gatePredictResult r;
    gateStateId st;
    gateReason why;
    int64_t now = T0, clock;
    uint64_t gen = 0, eligible, training, updates, fp;
    float w[16];
    gate *g;

    cfg_classifier(&c);
    g = mk(&c);
    CHECK(gateRecover(g, T0 - 5, 9) == GATE_ERR); /* nothing to recover from */
    CHECK(g->recoveries == 0 && g->epoch == EPOCH);
    train_ab(g, now);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    eligible = creq(g, ++now, 1, &VA).seq;
    training = creq(g, ++now, 0, &VB).seq;
    updates = g->trainer.updates;
    memcpy(w, g->trainer.w, sizeof(w));

    gateFence(g, ++now, 0x1111);
    INV(g);
    CHECK_STATE(g, GATE_STATE_REVOKED, GATE_REASON_RECOVERY_REQUIRED);
    CHECK(g->recovery_required == 1 && g->revoked_reason == GATE_REASON_RECOVERY_REQUIRED);
    CHECK(g->candidate == NULL && g->serving == NULL && g->npending == 0);
    CHECK(g->pending_bytes == 0 && g->invalid_below == g->next_seq);
    CHECK(g->epoch == 0x1111 && g->lineage == LINEAGE);
    CHECK(g->stats.candidates_discarded == 1 && g->stats.revocations == 0);
    /* The ledger and the trainer survive. */
    CHECK(g->generations == 1 && g->trainer.updates == updates);
    CHECK(memcmp(w, g->trainer.w, sizeof(w)) == 0);
    CHECK_NEAR(gateBudgetSpentNow(g), 0.05, 1e-15);

    /* Ids issued before the fence carry the old epoch: UNKNOWN. */
    CHECK_FB(gateLearn(g, now, EPOCH, eligible, &f, NULL), GATE_FB_UNKNOWN);
    CHECK_FB(gateLearn(g, now, EPOCH, training, &f, NULL), GATE_FB_UNKNOWN);
    CHECK_FB(gateLearn(g, now, 0x1111, eligible, &f, NULL), GATE_FB_INVALIDATED);
    CHECK(g->trainer.updates == updates);
    INV(g);

    r = creq(g, ++now, 1, &VA);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_RECOVERY_REQUIRED);
    CHECK(r.seq == 0 && g->npending == 0);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_ERR_RECOVERY);
    CHECK(g->generations == 1);

    /* Fencing again only changes the epoch. */
    fp = fingerprint(g, FP_ALL);
    gateFence(g, now, 0x1111);
    CHECK(fingerprint(g, FP_ALL) == fp);
    gateFence(g, now, 0x2222);
    CHECK(g->epoch == 0x2222 && g->stats.candidates_discarded == 1 && g->recoveries == 0);
    INV(g);

    /* The pause and a reset do not lift the fence. */
    gatePause(g, ++now);
    INV(g);
    CHECK_STATE(g, GATE_STATE_PAUSED, GATE_REASON_RECOVERY_REQUIRED);
    r = creq(g, ++now, 0, &VA);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_RECOVERY_REQUIRED);
    gateResume(g, ++now);
    gateReset(g, ++now, GATE_RESET_CALIBRATION);
    INV(g);
    CHECK_STATE(g, GATE_STATE_REVOKED, GATE_REASON_RECOVERY_REQUIRED);
    CHECK(g->recovery_required == 1);

    CHECK(gateRecover(g, ++now, 0x3333) == GATE_OK);
    INV(g);
    CHECK_STATE(g, GATE_STATE_COLD, GATE_REASON_COLD_START);
    CHECK(g->recovery_required == 0 && g->recoveries == 1 && g->epoch == 0x3333);
    CHECK(g->revoked_reason == GATE_REASON_NONE);
    /* Not twice; a refused recovery does not even move the clock. */
    clock = g->now_ms;
    CHECK(gateRecover(g, clock + 1000, 0x4444) == GATE_ERR);
    CHECK(g->recoveries == 1 && g->epoch == 0x3333 && g->now_ms == clock);

    /* A new lifecycle on the old ledger: generation 2, new epoch, the id
     * numbering goes on. */
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    CHECK(gen == 2);
    CHECK_NEAR(gateBudgetSpentNow(g), 0.1 * 2 / 3, 1e-15);
    r = creq(g, ++now, 1, &VA);
    CHECK(r.seq == 3);
    CHECK_FB(gateLearn(g, now, 0x1111, r.seq, &f, NULL), GATE_FB_UNKNOWN);
    CHECK_FB(gateLearn(g, now, 0x3333, r.seq, &f, NULL), GATE_FB_APPLIED);
    CHECK(g->candidate->samples == 1);
    INV(g);
    gateFree(g);

    /* Fencing a serving gate is a revocation, visible in the history, and
     * GINFO can preview it before it is applied. */
    cfg_score(&c);
    g = mk(&c);
    now = promote_everything(g, T0);
    gatePreview(g, now, 1, &st, &why);
    CHECK(st == GATE_STATE_REVOKED && why == GATE_REASON_RECOVERY_REQUIRED);
    CHECK(gateState(g) == GATE_STATE_ACTIVE);
    gateFence(g, ++now, 5);
    INV(g);
    CHECK(g->stats.revocations == 1 && g->last_revoked_reason == GATE_REASON_RECOVERY_REQUIRED);
    CHECK(g->last_revoked_gen == 1 && g->last_revoked_ms == now);
    r = req(g, ++now, 0, 1.0, -1);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_RECOVERY_REQUIRED);
    CHECK(gateRecover(g, ++now, 6) == GATE_OK);
    CHECK_STATE(g, GATE_STATE_COLD, GATE_REASON_COLD_START);
    CHECK(g->last_revoked_reason == GATE_REASON_RECOVERY_REQUIRED); /* history stays */
    INV(g);
    gateFree(g);
}

/* 15. The clock never goes backwards. */
TEST(clock_is_monotonic) {
    gateConfig c;
    gatePredictResult r;
    uint64_t gen;
    gate *g;

    cfg_score(&c);
    g = mk(&c);
    gateMaintain(g, T0 + 500);
    CHECK(g->now_ms == T0 + 500);
    gateMaintain(g, T0 + 100);
    CHECK(g->now_ms == T0 + 500);
    gatePause(g, T0 - 1000000);
    gateResume(g, 0);
    CHECK(g->now_ms == T0 + 500);

    /* A command with an older clock is stamped with the gate's clock. */
    CHECK_EQ_INT(gateFreeze(g, T0 + 200, &gen), GATE_FREEZE_OK);
    CHECK(g->candidate->frozen_ms == T0 + 500);
    r = req(g, T0 + 300, 1, 0.9, -1);
    CHECK(r.seq != 0 && gateFindObservation(g, r.seq)->created_ms == T0 + 500);
    /* Its TTL runs from that stamp, not from the stale argument. */
    gateMaintain(g, T0 + 300 + TTL);
    CHECK(g->npending == 1 && g->candidate != NULL);
    CHECK_EQ_INT(gateOldestPendingAge(g), TTL - 200);
    gateMaintain(g, T0 + 500 + TTL - 1);
    CHECK(g->npending == 1);
    gateMaintain(g, T0 + 500 + TTL);
    INV(g);
    CHECK(g->npending == 0 && g->candidate == NULL);
    CHECK_EQ_INT(gateOldestPendingAge(g), 0);
    CHECK_FB(fb(g, T0, r.seq, 1), GATE_FB_EXPIRED); /* an old clock does not revive it */
    CHECK(g->now_ms == T0 + 500 + TTL);
    gateFree(g);
}

/* 15. Byte accounting. */
TEST(memory_accounting) {
    static const vec five = {5, {1, 3, 4, 8, 15}, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f}};
    gateConfig c;
    gatePredictResult r;
    int64_t now = T0;
    uint64_t gen, base, model, genbytes, fixed, waiting, head;
    gate *g;

    cfg_classifier(&c);
    c.max_pending = 100;
    g = mk(&c);

    /* The footprint, term by term, from the shape of this gate: 2 labels
     * ("no", "yes"), two 10 and 9 byte versions, no class arrays, 4
     * thresholds, 1 row of bets, a 128 bucket index, a 17 float32 model. */
    base = sizeof(gate) + 2 * sizeof(char *) + 3 + 4 + 11 + 10 + 4 * sizeof(double) +
           sizeof(gateBets) + 128 * sizeof(gateObs *);
    model = 17 * sizeof(float);
    genbytes = sizeof(gateGeneration) + model + 4 * sizeof(gateRegion) + sizeof(int32_t) +
               sizeof(gateDriftWindow);
    fixed = base + model + 2 * genbytes;
    CHECK_EQ_INT(g->fixed_bytes, fixed);
    CHECK_EQ_INT(gateMemoryUsage(g), fixed - 2 * genbytes);
    CHECK_EQ_INT(gateFreeEffort(g), 1);

    now = classifier_promote(g, now);
    CHECK_EQ_INT(gateMemoryUsage(g), fixed - genbytes); /* serving */
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    CHECK_EQ_INT(gateMemoryUsage(g), fixed); /* serving + candidate: all of it */
    CHECK_EQ_INT(gateFreeEffort(g), 1 + 2);

    /* Observations: the struct plus 8 bytes per stored feature. */
    head = creq(g, ++now, 1, &VC).seq; /* eligible (candidate) */
    r = creq(g, ++now, 1, &five);      /* eligible, queued behind it */
    waiting = r.seq;
    creq(g, ++now, 0, &V0);            /* training only, no features */
    CHECK_EQ_INT(g->pending_bytes, 3 * sizeof(gateObs) + (1 + 5 + 0) * 8);
    CHECK_EQ_INT(gateMemoryUsage(g), fixed + g->pending_bytes);
    CHECK_EQ_INT(gateFreeEffort(g), 1 + 2 + 3);

    /* Labelled but still queued: the features are released at once, the
     * rest when its turn comes. */
    CHECK_FB(clearn(g, ++now, waiting, 1), GATE_FB_APPLIED);
    CHECK_EQ_INT(g->pending_bytes, 3 * sizeof(gateObs) + (1 + 0 + 0) * 8);
    CHECK(gateFindObservation(g, waiting) != NULL && gateFindObservation(g, waiting)->x.n == 0);
    CHECK_FB(clearn(g, ++now, head, 1), GATE_FB_APPLIED);
    CHECK_EQ_INT(g->pending_bytes, sizeof(gateObs));
    CHECK_EQ_INT(g->npending, 1);
    CHECK_EQ_INT(gateMemoryUsage(g), fixed + sizeof(gateObs));
    gateFree(g);
}

/* 15. gateFree() with everything alive (the leak check is the allocator
 * count at the end of the run, and valgrind). */
TEST(free_with_pending) {
    gateConfig c;
    int64_t now = T0;
    long before = live_blocks;
    uint64_t gen, seq;
    gate *g;
    int i;

    cfg_classifier(&c);
    g = mk(&c);
    now = classifier_promote(g, now);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    /* Odd i: audited, so always an observation (10). Even i: A is accepted
     * (6 times), C is below the threshold and kept for training (4). */
    for (i = 0; i < 20; i++) creq(g, ++now, i % 2, i % 3 ? &VA : &VC);
    seq = creq(g, ++now, 1, &VA).seq;
    CHECK_FB(clearn(g, ++now, seq, 1), GATE_FB_APPLIED); /* resolved, still queued */
    CHECK_EQ_INT(g->npending, 15);
    CHECK(g->candidate != NULL && g->serving != NULL);
    CHECK(gateFindObservation(g, seq) != NULL);
    CHECK(live_blocks > before + 15);
    gateFree(g);
    CHECK_EQ_INT(live_blocks, before);
}

/* Allocation failures: every one of them, one at a time. */
TEST(allocation_failures) {
    gateConfig c;
    gatePredictResult r;
    gateEval ev;
    gateSparse x;
    long start = live_blocks, base, k;
    uint64_t gen = 55, fp;
    int rc = -1;
    gate *g = NULL;

    cfg_classifier(&c);
    base = live_blocks;

    /* gateCreate(): NULL, with nothing left behind, whichever allocation
     * fails. */
    for (k = 1; k < 100; k++) {
        alloc_calls = 0;
        fail_at = k;
        g = gateCreate(&c, LINEAGE, EPOCH, T0);
        fail_at = 0;
        if (g != NULL) break;
        CHECK_EQ_INT(live_blocks, base);
    }
    /* 11 allocations: the gate, 5 blocks of configuration, the grid, the
     * bets, 2 trainer arrays, the index. */
    CHECK_EQ_INT(k, 12);
    if (g == NULL) exit(1);
    INV(g);

    /* gateFreeze(): refused, and the ledger is not charged. */
    CHECK(ctrain(g, T0, &VA, 1) == GATE_OK);
    fp = fingerprint(g, FP_ALL);
    for (k = 1; k < 100; k++) {
        long live = live_blocks;

        alloc_calls = 0;
        fail_at = k;
        rc = gateFreeze(g, T0, &gen);
        fail_at = 0;
        if (rc == GATE_FREEZE_OK) break;
        CHECK_EQ_INT(rc, GATE_FREEZE_ERR_NOMEM);
        CHECK(gen == 55 && g->generations == 0 && g->candidate == NULL);
        CHECK(fingerprint(g, FP_ALL) == fp);
        CHECK_EQ_INT(live_blocks, live);
        INV(g);
    }
    /* 6 allocations: the generation, its 3 arrays, 2 arrays of weights. */
    CHECK_EQ_INT(k, 7);
    CHECK(rc == GATE_FREEZE_OK && gen == 1 && g->generations == 1 && g->candidate != NULL);
    INV(g);

    /* gatePredict(): an observation that cannot be allocated is one that
     * cannot be stored. ONMISSING REVOKE: the candidate goes. */
    x = vec_sparse(&VA);
    gateMaintain(g, T0);
    CHECK(gateEvaluate(g, &x, 0.0, -1, &ev) == GATE_OK);
    alloc_calls = 0;
    fail_at = 1;
    CHECK(gatePredict(g, T0, 1, &ev, &x, &r) == GATE_OK);
    fail_at = 0;
    INV(g);
    CHECK_RES(r, GATE_ACTION_ABSTAIN, GATE_REASON_RESOURCE_LIMIT);
    CHECK(r.seq == 0 && x.n == 1 && x.idx != NULL); /* still the caller's */
    CHECK(g->candidate == NULL && g->npending == 0 && g->stats.resource_limited == 1);
    CHECK(g->stats.candidates_discarded == 1 && g->next_seq == 1);
    gateSparseFree(&x);

    /* gateSetTeacher(): refused before anything is invalidated. */
    CHECK_EQ_INT(gateFreeze(g, T0, &gen), GATE_FREEZE_OK);
    fp = fingerprint(g, FP_ALL);
    alloc_calls = 0;
    fail_at = 1;
    CHECK(gateSetTeacher(g, T0, "teacher-v2") == GATE_ERR);
    fail_at = 0;
    CHECK(fingerprint(g, FP_ALL) == fp);
    CHECK_EQ_STR(g->cfg.teacher, "teacher-v1");
    CHECK(g->candidate != NULL && g->trainer.updates == 1);
    INV(g);

    gateFree(g);
    gateConfigFree(&c);
    CHECK_EQ_INT(live_blocks, start);
}

/* Arguments the caller is supposed to have validated: refused without
 * touching the gate, never undefined behaviour. */
TEST(invalid_arguments) {
    static const vec out_of_range = {2, {3, 16}, {1.0f, 1.0f}};
    static const vec unordered = {2, {7, 7}, {1.0f, 1.0f}};
    gateConfig c;
    gatePredictResult r;
    gateFeedback f;
    gateEval ev;
    gateSparse x;
    double q[2] = {0.0, 1.0}, bad_q[2], probs[2] = {0.25, 0.75};
    int64_t now = T0;
    uint64_t gen, fp, seq;
    vec nonfinite = {1, {2}, {0.0f}};
    gate *g;

    nonfinite.val[0] = (float)INFINITY;
    bad_q[0] = bad_q[1] = NAN;

    /* SCORE gate. */
    cfg_score(&c);
    g = mk(&c);
    now = promote_everything(g, now);
    gateMaintain(g, now);
    fp = fingerprint(g, FP_ALL);
    memset(&ev, 0, sizeof(ev));
    CHECK(gatePredict(g, now, 0, &ev, NULL, &r) == GATE_ERR); /* no serving prediction */
    CHECK(gatePredict(g, now, 0, NULL, NULL, &r) == GATE_ERR);
    ev.has_serving = 1;
    ev.serving.label = -1;
    ev.serving.score = NAN;
    CHECK(gatePredict(g, now, 0, &ev, NULL, &r) == GATE_ERR);
    ev.serving.score = INFINITY;
    CHECK(gatePredict(g, now, 0, &ev, NULL, &r) == GATE_ERR);
    ev.serving.score = 0.9;
    CHECK(gatePredict(g, now, 0, &ev, NULL, NULL) == GATE_ERR);
    CHECK(gateEvaluate(g, NULL, NAN, -1, &ev) == GATE_ERR);
    CHECK(ev.has_serving == 0 && ev.has_candidate == 0);
    CHECK(fingerprint(g, FP_ALL) == fp);
    /* A gate without classes has no labels: whatever is passed is ignored. */
    ev.has_serving = 1;
    ev.serving.label = 7;
    ev.serving.score = 0.9;
    ev.has_candidate = 1; /* a prediction for a generation that does not exist */
    ev.candidate.label = 99;
    ev.candidate.score = NAN;
    CHECK(gatePredict(g, now, 1, &ev, NULL, &r) == GATE_OK);
    INV(g);
    CHECK_RES(r, GATE_ACTION_AUDIT, GATE_REASON_NONE);
    CHECK(r.label == -1);
    seq = r.seq;
    CHECK(gateFindObservation(g, seq)->serv_label == -1);
    CHECK(gateFindObservation(g, seq)->cand_gen == 0);

    /* Feedback of the wrong kind or value: the observation stays pending. */
    fp = fingerprint(g, FP_NO_FEEDBACK);
    f.kind = GATE_FEEDBACK_LABEL;
    f.label = 0;
    f.probs = NULL;
    f.correct = 1;
    CHECK_FB(gateLearn(g, now, g->epoch, seq, &f, NULL), GATE_FB_UNKNOWN);
    f.kind = GATE_FEEDBACK_CORRECT;
    f.correct = 2;
    CHECK_FB(gateLearn(g, now, g->epoch, seq, &f, NULL), GATE_FB_UNKNOWN);
    CHECK_FB(gateLearn(g, now, g->epoch, seq, NULL, NULL), GATE_FB_UNKNOWN);
    CHECK(fingerprint(g, FP_NO_FEEDBACK) == fp);
    CHECK_EQ_INT(g->stats.feedback[GATE_FB_UNKNOWN], 3);
    f.correct = 1;
    CHECK_FB(gateLearn(g, now, g->epoch, seq, &f, NULL), GATE_FB_APPLIED);
    INV(g);
    /* GADD on a gate that has no trainer. */
    x = vec_sparse(&VA);
    CHECK(gateTrain(g, now, &x, q) == GATE_ERR);
    CHECK(g->stats.train_rejected == 1);
    gateSparseFree(&x);
    INV(g);
    gateFree(g);

    /* CLASSIFIER gate. */
    cfg_classifier(&c);
    g = mk(&c);
    CHECK(ctrain(g, ++now, &VA, 1) == GATE_OK);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    fp = fingerprint(g, FP_ALL);

    /* Malformed feature vectors. */
    x = vec_sparse(&out_of_range);
    CHECK(gateEvaluate(g, &x, 0.0, -1, &ev) == GATE_ERR);
    ev.has_candidate = 1;
    ev.candidate.label = 1;
    ev.candidate.score = 0.7;
    CHECK(gatePredict(g, now, 1, &ev, &x, &r) == GATE_ERR);
    CHECK(x.n == 2 && x.idx != NULL);
    CHECK(fingerprint(g, FP_ALL) == fp);
    CHECK(gateTrain(g, now, &x, q) == GATE_ERR);
    gateSparseFree(&x);
    x = vec_sparse(&unordered);
    CHECK(gatePredict(g, now, 1, &ev, &x, &r) == GATE_ERR);
    CHECK(gateTrain(g, now, &x, q) == GATE_ERR);
    gateSparseFree(&x);
    x = vec_sparse(&nonfinite);
    CHECK(gatePredict(g, now, 1, &ev, &x, &r) == GATE_ERR);
    CHECK(gateTrain(g, now, &x, q) == GATE_ERR);
    gateSparseFree(&x);
    CHECK(gateEvaluate(g, NULL, 0.0, -1, &ev) == GATE_ERR); /* a model needs features */
    x = vec_sparse(&VA);
    CHECK(gateTrain(g, now, &x, NULL) == GATE_ERR);
    CHECK(gateTrain(g, now, &x, bad_q) == GATE_ERR); /* the SGD step itself is rejected */
    CHECK(gateTrain(g, now, NULL, q) == GATE_ERR);
    gateSparseFree(&x);
    CHECK_EQ_INT(g->stats.train_rejected, 6);
    g->stats.train_rejected = 0;
    CHECK(fingerprint(g, FP_ALL) == fp);
    INV(g);

    /* A prediction for the candidate is required once it exists. */
    memset(&ev, 0, sizeof(ev));
    x = vec_sparse(&VA);
    CHECK(gatePredict(g, now, 1, &ev, &x, &r) == GATE_ERR);
    ev.has_candidate = 1;
    ev.candidate.score = 0.7;
    ev.candidate.label = 2; /* not a class */
    CHECK(gatePredict(g, now, 1, &ev, &x, &r) == GATE_ERR);
    CHECK(gatePredictNeedsFeatures(g, 0, &ev) == 1);
    CHECK(fingerprint(g, FP_ALL) == fp);
    ev.candidate.label = 1;
    CHECK(gatePredict(g, now, 1, &ev, &x, &r) == GATE_OK);
    INV(g);
    seq = r.seq;
    CHECK(seq != 0 && x.n == 0);

    /* A label that is not a class, a missing distribution. */
    fp = fingerprint(g, FP_NO_FEEDBACK);
    f.kind = GATE_FEEDBACK_LABEL;
    f.probs = NULL;
    f.correct = 0;
    f.label = 2;
    CHECK_FB(gateLearn(g, now, g->epoch, seq, &f, NULL), GATE_FB_UNKNOWN);
    f.label = -1;
    CHECK_FB(gateLearn(g, now, g->epoch, seq, &f, NULL), GATE_FB_UNKNOWN);
    f.label = 1;
    f.kind = GATE_FEEDBACK_PROBS;
    CHECK_FB(gateLearn(g, now, g->epoch, seq, &f, NULL), GATE_FB_UNKNOWN);
    f.kind = GATE_FEEDBACK_CORRECT;
    CHECK_FB(gateLearn(g, now, g->epoch, seq, &f, NULL), GATE_FB_UNKNOWN);
    CHECK(fingerprint(g, FP_NO_FEEDBACK) == fp);

    /* A soft target the SGD step rejects: the label is still evidence, the
     * trainer is untouched, and the rejection is counted. */
    f.kind = GATE_FEEDBACK_PROBS;
    f.probs = bad_q;
    f.label = 1;
    CHECK_FB(gateLearn(g, now, g->epoch, seq, &f, NULL), GATE_FB_APPLIED);
    INV(g);
    CHECK(g->stats.train_rejected == 1 && g->trainer.updates == 1);
    CHECK(g->candidate->samples == 1 && g->candidate->errors == 0 && g->npending == 0);

    /* A proper soft target trains towards the distribution and counts as
     * its argmax. */
    x = vec_sparse(&VB);
    gateMaintain(g, now);
    CHECK(gateEvaluate(g, &x, 0.0, -1, &ev) == GATE_OK);
    CHECK(gatePredict(g, now, 1, &ev, &x, &r) == GATE_OK);
    f.probs = probs;
    f.label = 1;
    CHECK_FB(gateLearn(g, now, g->epoch, r.seq, &f, NULL), GATE_FB_APPLIED);
    INV(g);
    CHECK(g->trainer.updates == 2 && g->candidate->samples == 2);
    gateFree(g);
}

/* gateEvaluate(). */
TEST(evaluate) {
    gateConfig c;
    gateSparse xa = vec_sparse(&VA), xc = vec_sparse(&VC);
    gateEval ev;
    double s_serv, s_cand;
    int64_t now = T0;
    uint64_t gen;
    gate *g;
    int l_serv, l_cand, i;

    /* SCORE: both generations "predict" what the application passed. */
    cfg_score(&c);
    g = mk(&c);
    CHECK(gateEvaluate(g, NULL, 0.4, 5, &ev) == GATE_OK);
    CHECK(ev.has_serving == 0 && ev.has_candidate == 0);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    CHECK(gateEvaluate(g, NULL, 0.4, 5, &ev) == GATE_OK);
    CHECK(ev.has_serving == 0 && ev.has_candidate == 1);
    CHECK(ev.candidate.score == 0.4 && ev.candidate.label == -1); /* no classes: no label */
    gateFree(g);
    cfg_classes(&c);
    g = mk(&c);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    for (i = 0; i < 32; i++) sample(g, ++now, 1.0, 0, 1);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK);
    CHECK(gateEvaluate(g, NULL, 0.4, 1, &ev) == GATE_OK);
    CHECK(ev.has_serving == 1 && ev.has_candidate == 1);
    CHECK(ev.serving.label == 1 && ev.serving.score == 0.4);
    CHECK(ev.candidate.label == 1 && ev.candidate.score == 0.4);
    gateFree(g);

    /* CLASSIFIER: each generation answers with its own frozen weights. */
    cfg_classifier(&c);
    g = mk(&c);
    now = classifier_promote(g, T0); /* generation 1 says "yes" for A */
    for (i = 0; i < 50; i++) CHECK(ctrain(g, now, &VA, 0) == GATE_OK);
    CHECK_EQ_INT(gateFreeze(g, ++now, &gen), GATE_FREEZE_OK); /* generation 2: "no" */
    l_serv = gateModelPredict(&g->serving->model, &xa, NULL, &s_serv);
    l_cand = gateModelPredict(&g->candidate->model, &xa, NULL, &s_cand);
    CHECK(l_serv == 1 && l_cand == 0);
    CHECK(gateEvaluate(g, &xa, 0.123, 7, &ev) == GATE_OK); /* ext_* are ignored */
    CHECK(ev.has_serving == 1 && ev.serving.label == 1 && ev.serving.score == s_serv);
    CHECK(ev.has_candidate == 1 && ev.candidate.label == 0 && ev.candidate.score == s_cand);
    CHECK(xa.n == 1 && xa.idx != NULL); /* pure: the vector is the caller's */
    l_serv = gateModelPredict(&g->serving->model, &xc, NULL, &s_serv);
    CHECK(gateEvaluate(g, &xc, 0.0, -1, &ev) == GATE_OK);
    CHECK(ev.serving.label == l_serv && ev.serving.score == s_serv);
    gateSparseFree(&xa);
    gateSparseFree(&xc);
    gateFree(g);
}

/* 16. Randomized soak: thousands of random operations, every documented
 * consequence that can be checked from the outside checked after each one. */

/* The status the header's rules give for this feedback, evaluated on the
 * gate as it is right before the call. */
static gateFeedbackStatus expected_status(const gate *g, uint64_t epoch, uint64_t seq,
                                          const char *teacher)
{
    const gateObs *o;

    if (epoch != g->epoch || seq == 0 || seq >= g->next_seq) return GATE_FB_UNKNOWN;
    o = gateFindObservation(g, seq);
    if (o == NULL) {
        if (seq < g->invalid_below) return GATE_FB_INVALIDATED;
        if (seq <= g->expired_through) return GATE_FB_EXPIRED;
        return GATE_FB_DUPLICATE;
    }
    if (o->flags & GATE_OBS_RESOLVED) return GATE_FB_DUPLICATE;
    if (teacher != NULL && strcmp(teacher, g->cfg.teacher) != 0) return GATE_FB_TEACHER_MISMATCH;
    return GATE_FB_APPLIED;
}

typedef struct soakTally {
    uint64_t accept, audit, abstain, resource_limit;
    uint64_t promotions, revocations, discarded, expired, imputed, freezes, recoveries;
    uint64_t status[GATE_FB_STATUS_COUNT];
    uint64_t revoked_for[GATE_REASON_COUNT];
} soakTally;

#define SOAK_TAG 0x7a67a67a67ull
#define SOAK_TTL 400

static void soak_config(int variant, gateConfig *c) {
    if (variant == 0) { /* SCORE, pooled, specification behaviour */
        cfg_score(c);
        c->max_pending = 64;
    } else if (variant == 1) { /* SCORE, class-conditional, forgiving, small table */
        cfg_classes(c);
        c->on_missing = GATE_ONMISSING_ERROR;
        c->expand = 1;
        c->max_pending = 8;
    } else if (variant == 2) { /* CLASSIFIER, REVOKE + EXPAND */
        cfg_classifier(c);
        c->expand = 1;
        c->max_pending = 64;
        c->lr = 0.5;
    } else { /* CLASSIFIER, ERROR, a tight byte budget, L2 */
        cfg_classifier(c);
        c->on_missing = GATE_ONMISSING_ERROR;
        c->max_pending = 64;
        c->lr = 0.5;
        c->l2 = 0.001;
        c->max_memory = gateFixedBytes(c) + 12 * GATE_OBS_BYTES(2);
    }
    c->observation_ttl_ms = SOAK_TTL;
    c->cert_ttl_ms = 30000;
    c->audit = 0.5;
    c->cold_audit = 0.5;
}

static uint64_t soak_run(int variant, uint64_t seed, int nops, soakTally *tally) {
    static const char *const teachers[2] = {"teacher-v1", "teacher-v2"};
    static const char *const schemas[2] = {"schema-v1", "schema-v2"};
    gateConfig c;
    uint64_t rng = seed, fp;
    int64_t now = T0;
    uint8_t *audited, *truth, *consumed; /* per issued id */
    int classifier, failures = gate_test_failures, op;
    gate *g;

    memset(tally, 0, sizeof(*tally));
    soak_config(variant, &c);
    g = mk(&c);
    g->runtime_tag = SOAK_TAG;
    classifier = g->cfg.mode == GATE_MODE_CLASSIFIER;
    audited = calloc((size_t)nops + 2, 1);
    truth = calloc((size_t)nops + 2, 1);
    consumed = calloc((size_t)nops + 2, 1);

    for (op = 0; op < nops && gate_test_failures == failures; op++) {
        int chaotic = (op / 500) % 3 == 2;       /* resets, config changes, fences */
        int drifting = op >= 3500 && op < 4500;  /* the cheap answers go bad */
        uint64_t roll = gate_test_rand64(&rng) % 100;
        uint64_t gens0 = g->generations, seq0 = g->next_seq, epoch0 = g->epoch;
        int64_t clock0 = g->now_ms;
        gateCounters s0;
        int reset_all = 0, epoch_may_change = 0;

        now += (int64_t)(gate_test_rand64(&rng) % (chaotic ? 40 : 6));
        if (chaotic && gate_test_rand64(&rng) % 100 == 0) now += 2 * SOAK_TTL;
        s0 = g->stats;

        if (roll < 40) {
            /* ---- GPREDICT ---- */
            gatePredictResult r;
            gateStateId before;
            gateSparse x = {0, NULL, NULL};
            gateEval ev;
            double score = 0.0;
            int32_t label = -1, answer;
            int audit, needs, had_generation, recovering;

            if (classifier) {
                /* Six inputs; the teacher's label is the parity of the first
                 * feature, except for noise, and the opposite when drifting. */
                vec v = {2, {0, 10}, {1.0f, 0.5f}};
                v.idx[0] = (uint32_t)(gate_test_rand64(&rng) % 6);
                x = vec_sparse(&v);
                answer = (int32_t)(v.idx[0] % 2);
                if (drifting || gate_test_rand64(&rng) % 100 < 3) answer = 1 - answer;
            } else {
                double u = gate_test_uniform(&rng);
                score = gate_test_rand64(&rng) % 5 ? 0.5 + 0.5 * u : 0.5 * u;
                if (g->cfg.nclasses != 0) label = (int32_t)(gate_test_rand64(&rng) % 3);
                /* CORRECT: nearly always above 0.5 (40% when drifting), a
                 * coin flip below. */
                if (score >= 0.5)
                    answer = gate_test_rand64(&rng) % 100 < (drifting ? 40u : 97u);
                else
                    answer = (int32_t)(gate_test_rand64(&rng) % 2);
            }
            gateMaintain(g, now);
            before = gateState(g);
            had_generation = g->serving != NULL || g->candidate != NULL;
            recovering = g->recovery_required;
            audit = gate_test_uniform(&rng) < gateAuditProbability(g);
            CHECK(gateEvaluate(g, classifier ? &x : NULL, score, label, &ev) == GATE_OK);
            needs = gatePredictNeedsFeatures(g, audit, &ev);
            CHECK(gatePredict(g, now, audit, &ev, classifier ? &x : NULL, &r) == GATE_OK);
            gateSparseFree(&x);

            if (r.action == GATE_ACTION_ACCEPT) {
                tally->accept++;
                CHECK(before == GATE_STATE_ACTIVE && !audit && r.seq == 0 && r.generation != 0);
                CHECK(needs == 0);
            } else if (r.action == GATE_ACTION_AUDIT) {
                tally->audit++;
                CHECK(before == GATE_STATE_ACTIVE && audit && r.seq != 0 && r.generation != 0);
            } else {
                tally->abstain++;
                CHECK(r.action == GATE_ACTION_ABSTAIN && r.reason != GATE_REASON_NONE);
                if (r.reason == GATE_REASON_RESOURCE_LIMIT) {
                    tally->resource_limit++;
                    CHECK(r.seq == 0);
                }
            }
            if (before != GATE_STATE_ACTIVE) CHECK(r.action == GATE_ACTION_ABSTAIN);
            if (r.seq != 0) {
                CHECK(r.seq == seq0 && g->next_seq == seq0 + 1);
                audited[r.seq] = (uint8_t)audit;
                truth[r.seq] = (uint8_t)answer;
            } else {
                CHECK(g->next_seq == seq0);
            }
            /* SCORE: an id only for sampled requests with something to test.
             * CLASSIFIER: an id for everything that is not accepted. */
            if (!classifier && (!audit || !had_generation)) CHECK(r.seq == 0);
            if (classifier && r.action != GATE_ACTION_ACCEPT && !recovering &&
                r.reason != GATE_REASON_RESOURCE_LIMIT)
                CHECK(r.seq != 0);
        } else if (roll < 80) {
            /* ---- GLEARN ---- */
            double probs[2];
            gateFeedback f;
            gateFeedbackStatus st, want;
            const gateGeneration *cand0, *serv0;
            uint64_t seq, epoch = g->epoch, pick = gate_test_rand64(&rng) % 100;
            uint64_t cand_samples = 0, serv_samples = 0;
            const char *teacher = NULL;

            gateMaintain(g, now);
            /* A well behaved client answers the oldest request first; the
             * rest of the time any recent id, pending or not. */
            if (pick < 70 && g->oldest != NULL) {
                seq = g->oldest->seq;
            } else {
                uint64_t lo = g->next_seq > 100 ? g->next_seq - 100 : 0;
                seq = lo + gate_test_rand64(&rng) % (g->next_seq - lo + 3);
            }
            pick = gate_test_rand64(&rng) % 100;
            if (pick < 4) teacher = "somebody-else";
            else if (pick < 7) epoch ^= 0x10;
            else if (pick < 20) teacher = g->cfg.teacher;

            memset(&f, 0, sizeof(f));
            if (classifier) {
                f.label = seq < (uint64_t)nops + 2 ? truth[seq] : 0;
                f.kind = GATE_FEEDBACK_LABEL;
                if (gate_test_rand64(&rng) % 5 == 0) { /* a soft target */
                    probs[f.label] = 0.8;
                    probs[1 - f.label] = 0.2;
                    f.probs = probs;
                    f.kind = GATE_FEEDBACK_PROBS;
                }
            } else {
                f.kind = GATE_FEEDBACK_CORRECT;
                f.label = -1;
                f.correct = seq < (uint64_t)nops + 2 ? truth[seq] : 0;
            }
            want = expected_status(g, epoch, seq, teacher);
            cand0 = g->candidate;
            serv0 = g->serving;
            if (cand0 != NULL) cand_samples = cand0->samples;
            if (serv0 != NULL) serv_samples = serv0->samples;
            st = gateLearn(g, now, epoch, seq, &f, teacher);
            CHECK_FB(st, want);
            tally->status[st]++;
            if (st == GATE_FB_APPLIED) {
                CHECK(consumed[seq] == 0); /* an id is consumed at most once */
                consumed[seq] = 1;
                if (!audited[seq]) {
                    /* Never sampled: never evidence, for anything. */
                    CHECK(g->candidate == cand0 && g->serving == serv0);
                    CHECK(cand0 == NULL || cand0->samples == cand_samples);
                    CHECK(serv0 == NULL || serv0->samples == serv_samples);
                }
            }
        } else if (!chaotic && roll >= 84) {
            /* ---- GADD, or just the clock (sometimes an old one) ---- */
            if (classifier && roll < 92) {
                vec v = {2, {0, 10}, {1.0f, 0.5f}};
                v.idx[0] = (uint32_t)(gate_test_rand64(&rng) % 6);
                CHECK(ctrain(g, now, &v, (int32_t)(v.idx[0] % 2)) == GATE_OK);
            } else {
                gateMaintain(g, now - (int64_t)(gate_test_rand64(&rng) % 50));
            }
        } else if (roll < 84) {
            /* ---- GFREEZE ---- */
            uint64_t gen = 0;
            int want, rc;

            gateMaintain(g, now);
            want = g->recovery_required ? GATE_FREEZE_ERR_RECOVERY
                 : g->candidate != NULL ? GATE_FREEZE_ERR_CANDIDATE
                 : classifier && g->trainer.updates == 0 ? GATE_FREEZE_ERR_UNTRAINED
                 : GATE_FREEZE_OK;
            rc = gateFreeze(g, now, &gen);
            CHECK_EQ_INT(rc, want);
            if (rc == GATE_FREEZE_OK) {
                tally->freezes++;
                CHECK(gen == gens0 + 1 && g->generations == gen && g->candidate != NULL);
                CHECK(g->candidate != NULL && g->candidate->gen == gen);
            } else {
                CHECK(g->generations == gens0);
            }
        } else if (roll < 87) {
            /* ---- GRESET ---- */
            uint64_t scope = gate_test_rand64(&rng) % 8;
            gateResetScope s = scope < 4 ? GATE_RESET_CANDIDATE
                             : scope < 6 ? GATE_RESET_CALIBRATION
                             : scope < 7 ? GATE_RESET_MODEL : GATE_RESET_ALL;

            gateReset(g, now, s);
            reset_all = s == GATE_RESET_ALL;
            CHECK(g->candidate == NULL);
            if (s != GATE_RESET_CANDIDATE) CHECK(g->serving == NULL);
            if (s == GATE_RESET_MODEL || s == GATE_RESET_ALL) CHECK(g->trainer.updates == 0);
            if (reset_all) CHECK(g->npending == 0 && g->stats.requests == 0);
        } else if (roll < 90) {
            /* ---- GPAUSE / GRESUME ---- */
            if (gate_test_rand64(&rng) % 2) gatePause(g, now); else gateResume(g, now);
        } else if (roll < 92) {
            /* ---- GCONFIG SET AUDIT ---- */
            double q = gate_test_rand64(&rng) % 2 ? 0.5 : 0.6;
            int same = q == g->cfg.audit;
            const gateGeneration *cand0;

            gateMaintain(g, now);
            cand0 = g->candidate;
            gateSetAudit(g, now, q);
            CHECK(g->cfg.audit == q);
            if (same) CHECK(g->candidate == cand0);
            else CHECK(g->candidate == NULL && g->serving == NULL);
        } else if (roll < 94) {
            /* ---- GCONFIG SET TEACHER / SCHEMAVERSION ---- */
            const char *name;
            int same;

            if (gate_test_rand64(&rng) % 2) {
                name = teachers[gate_test_rand64(&rng) % 2];
                same = strcmp(name, g->cfg.teacher) == 0;
                CHECK(gateSetTeacher(g, now, name) == GATE_OK);
                CHECK_EQ_STR(g->cfg.teacher, name);
            } else {
                name = schemas[gate_test_rand64(&rng) % 2];
                same = strcmp(name, g->cfg.schema_version) == 0;
                CHECK(gateSetSchemaVersion(g, now, name) == GATE_OK);
                CHECK_EQ_STR(g->cfg.schema_version, name);
            }
            if (!same) {
                CHECK(g->candidate == NULL && g->serving == NULL && g->npending == 0);
                CHECK(g->trainer.updates == 0 && g->invalid_below == g->next_seq);
            }
        } else if (roll < 95) {
            /* ---- failover: fence ---- */
            gateFence(g, now, gate_test_rand64(&rng));
            epoch_may_change = 1;
            CHECK(g->recovery_required && g->candidate == NULL && g->serving == NULL);
            CHECK(g->npending == 0);
        } else if (roll < 98) {
            /* ---- GRECOVER ---- */
            int required = g->recovery_required;
            uint32_t recoveries = g->recoveries;
            int rc = gateRecover(g, now, gate_test_rand64(&rng));

            epoch_may_change = 1;
            CHECK(rc == (required ? GATE_OK : GATE_ERR));
            CHECK(g->recoveries == recoveries + (uint32_t)required && !g->recovery_required);
            if (!required) CHECK(g->epoch == epoch0);
            tally->recoveries += (uint64_t)required;
        } else if (classifier) {
            vec v = {2, {0, 10}, {1.0f, 0.5f}};
            v.idx[0] = (uint32_t)(gate_test_rand64(&rng) % 6);
            CHECK(ctrain(g, now, &v, (int32_t)(v.idx[0] % 2)) == GATE_OK);
        } else {
            gateMaintain(g, now);
        }

        INV(g);
        /* What no operation may ever do. */
        CHECK(g->generations >= gens0 && g->next_seq >= seq0);
        CHECK(g->lineage == LINEAGE && g->runtime_tag == SOAK_TAG);
        CHECK(epoch_may_change || g->epoch == epoch0);
        CHECK(gateMemoryUsage(g) <= g->cfg.max_memory);
        CHECK(gateBudgetSpentNow(g) < g->cfg.delta);
        CHECK(g->now_ms >= clock0 && g->now_ms <= now);
        if (!reset_all) {
            tally->promotions += g->stats.promotions - s0.promotions;
            tally->discarded += g->stats.candidates_discarded - s0.candidates_discarded;
            tally->expired += g->stats.expired - s0.expired;
            tally->imputed += g->stats.imputed - s0.imputed;
            if (g->stats.revocations != s0.revocations) {
                tally->revocations += g->stats.revocations - s0.revocations;
                tally->revoked_for[g->last_revoked_reason]++;
            }
        }
    }
    if (gate_test_failures != failures)
        printf("    soak variant %d stopped after operation %d\n", variant, op);

    fp = fingerprint(g, FP_ALL);
    gateFree(g);
    free(audited);
    free(truth);
    free(consumed);
    return fp;
}

#define SOAK_OPS 6000

TEST(soak) {
    static const uint64_t seeds[4] = {11, 22, 33, 44};
    soakTally t[4], again;
    int v;

    for (v = 0; v < 4; v++) {
        uint64_t fp = soak_run(v, seeds[v], SOAK_OPS, &t[v]);

        printf("    soak %d: accept %llu audit %llu abstain %llu (limit %llu) | promoted %llu "
               "revoked %llu discarded %llu | expired %llu imputed %llu | applied %llu dup %llu "
               "expired %llu invalid %llu unknown %llu mismatch %llu\n", v,
               (unsigned long long)t[v].accept, (unsigned long long)t[v].audit,
               (unsigned long long)t[v].abstain, (unsigned long long)t[v].resource_limit,
               (unsigned long long)t[v].promotions, (unsigned long long)t[v].revocations,
               (unsigned long long)t[v].discarded, (unsigned long long)t[v].expired,
               (unsigned long long)t[v].imputed,
               (unsigned long long)t[v].status[GATE_FB_APPLIED],
               (unsigned long long)t[v].status[GATE_FB_DUPLICATE],
               (unsigned long long)t[v].status[GATE_FB_EXPIRED],
               (unsigned long long)t[v].status[GATE_FB_INVALIDATED],
               (unsigned long long)t[v].status[GATE_FB_UNKNOWN],
               (unsigned long long)t[v].status[GATE_FB_TEACHER_MISMATCH]);

        /* The determinism contract: the same calls with the same arguments
         * reach the same state, bit for bit. */
        CHECK(soak_run(v, seeds[v], SOAK_OPS, &again) == fp);
        CHECK(memcmp(&again, &t[v], sizeof(again)) == 0);
        /* And a different stream reaches a different one. */
        CHECK(soak_run(v, seeds[v] + 1, 500, &again) != fp);
    }
}

int main(void) {
    gateSetAllocator(t_malloc, t_calloc, t_realloc, t_free);

    RUN(expected_steps);
    RUN(constructors);
    RUN(lifecycle_and_ledger);
    RUN(freeze_errors_in_order);
    RUN(decisions_and_counters);
    RUN(paused_gate_keeps_calibrating);
    RUN(observation_rules_score);
    RUN(observation_rules_classifier);
    RUN(evidence_uses_frozen_predictions);
    RUN(evidence_is_bound_to_its_generation);
    RUN(evidence_waits_for_older_observations);
    RUN(arrival_order_does_not_select_the_stream);
    RUN(feedback_statuses);
    RUN(missing_label_revoke);
    RUN(missing_label_error);
    RUN(resource_limit_pending);
    RUN(resource_limit_memory);
    RUN(expand);
    RUN(expand_adds_classes);
    RUN(class_conditional);
    RUN(drift_guard);
    RUN(certificate_expiry);
    RUN(background_candidate);
    RUN(resets);
    RUN(config_changes);
    RUN(fence_and_recover);
    RUN(clock_is_monotonic);
    RUN(memory_accounting);
    RUN(free_with_pending);
    RUN(allocation_failures);
    RUN(invalid_arguments);
    RUN(evaluate);
    RUN(soak);

    /* Every block the library allocated during the run was released. */
    CHECK_EQ_INT(live_blocks, 0);
    gateSetAllocator(malloc, calloc, realloc, free);
    return TEST_REPORT();
}
