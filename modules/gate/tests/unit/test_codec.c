/*
 * Gate data type: unit tests of the binary codec (gate_codec.c).
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * WHAT THE CODEC IS COMPARED WITH. A codec that is only compared with
 * itself can be wrong in both directions at once, so:
 *
 *   - Three reference blobs (a score gate that answers, a classifier gate
 *     in which every kind of field has a value of its own, and a blind
 *     score gate) were written by a separate encoder: golden_codec.py, next
 *     to this file, follows the layout comment of gate_codec.c with
 *     struct.pack and never runs the C code. The same three gates are built
 *     here by hand. gateEncode() must produce those bytes and gateDecode()
 *     of those bytes must produce those gates. The numbers checked next to
 *     them (bars, thresholds, the lowest proven region) follow from the
 *     rules of gate_core.h, by hand or from that script.
 *   - layout_of() derives where every section of a blob starts from the
 *     layout comment alone. Every blob encoded here must have that size, a
 *     digest must differ from the plain encoding in exactly the bytes the
 *     header names, and hostile blobs are patched at those offsets.
 *
 * The rest drives gates through the real API into every state of their
 * life, takes them through the codec and compares state and behaviour:
 * requests waiting for an answer, answers waiting for an older answer,
 * requests the proof has moved on without, gates that went blind and gates
 * that can see again. One test takes a gate through the codec after every
 * operation of a long random history (every few for the larger blobs):
 * whatever the core can leave behind must decode.
 * Then the tests attack the decoder: hostile blobs made by hand, every
 * truncation, failing allocations, random corruption. A corrupted blob
 * that decodes all the same must also be safe to use: it is given to the
 * core to work on.
 *
 * Usage: test_codec [number of random corruptions [seed]]. The default is
 * 200000 corruptions with seed 0; a smaller number keeps a run under
 * valgrind short, another seed tries other corruptions. The length of the
 * random history follows the number of corruptions.
 */

#include <float.h>
#include "testutil.h"
#include "gate_codec.h"

/* An expectation inside a loop that runs thousands of times: counted and
 * reported only when it fails. */
#define MUST(cond) do { \
    if (!(cond)) { \
        gate_test_checks++; \
        gate_test_failures++; \
        printf("  %s:%d: in %s: MUST(%s) failed\n", __FILE__, __LINE__, \
               gate_test_current, #cond); \
    } \
} while (0)

#define COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

/* The last value of the clock (TIME in gate_core.h). */
#define LAST_MS (INT64_MAX - GATE_MAX_FEEDBACK_TIMEOUT_MS)

/* --------------------------------- Blobs --------------------------------- */

typedef struct blob {
    unsigned char *p;
    size_t n;
} blob;

static blob encode(const gate *g, int for_digest) {
    blob b = {NULL, 0};

    MUST(gateEncode(g, for_digest, &b.p, &b.n) == GATE_OK && b.p != NULL);
    return b;
}

static void blob_free(blob *b) {
    gate_free(b->p);
    b->p = NULL;
    b->n = 0;
}

static int blob_equal(const blob *a, const blob *b) {
    return a->p && b->p && a->n == b->n && memcmp(a->p, b->p, a->n) == 0;
}

/* Decode a blob that is expected to be valid, and say why if it is not. */
static gate *decode(const blob *b) {
    char err[160] = "";
    gate *g = gateDecode(b->p, b->n, err, sizeof(err));

    if (g == NULL) printf("  unexpected decoding failure: %s\n", err);
    return g;
}

static gate *clone(const gate *g) {
    blob b = encode(g, 0);
    gate *copy = decode(&b);

    blob_free(&b);
    return copy;
}

static void put(unsigned char *p, uint64_t v, size_t width) {
    for (size_t i = 0; i < width; i++) p[i] = (unsigned char)(v >> (8 * i));
}

/* --------------------------------- Layout --------------------------------
 * Sizes and offsets, from the layout comment of gate_codec.c. */

#define HEADER_BYTES 8
#define LEDGER_BYTES 54     /* five 64-bit fields, the pause flag, one more,
                             * the blind flag, a 32-bit run */
#define GENERATION_BYTES 72 /* nine 64-bit fields */
#define REGION_BYTES 56     /* four log wealths, three counters */
#define DRIFT_BYTES 32      /* two log statistics, two counters */
#define REQUEST_BYTES 68    /* without its clues, eight bytes each */
#define COUNTERS_BYTES 120  /* fifteen 64-bit fields */

/* Offsets inside the ledger and inside a request. */
#define LEDGER_NOW 24
#define LEDGER_PAUSED 40
#define LEDGER_BLIND 49
#define LEDGER_MISSING_RUN 50
#define REQUEST_CREATED 8
#define REQUEST_FLAGS 16
#define REQUEST_RESULT 60
#define REQUEST_NCLUES 64

typedef struct layout {
    size_t ledger;
    size_t trainer;   /* its presence byte */
    size_t candidate; /* its presence byte */
    size_t serving;   /* its presence byte */
    size_t requests;  /* next_seq, the count, then the requests */
    size_t counters;
    size_t end;
} layout;

static size_t model_bytes(const gate *g) {
    size_t k = g->cfg.nclasses, v = k == 2 ? 1 : k;

    return g->cfg.kind == GATE_KIND_CLASSIFIER ? 8 + v * GATE_DIM * 4 + v * 4 : 0;
}

static size_t generation_bytes(const gate *g, const gateGeneration *gen) {
    if (gen == NULL) return 1;
    return 1 + GENERATION_BYTES + model_bytes(g) +
           (size_t)g->nrows * (GATE_THRESHOLDS * REGION_BYTES + DRIFT_BYTES);
}

static layout layout_of(const gate *g) {
    size_t k = g->cfg.nclasses;
    layout l;

    l.ledger = HEADER_BYTES + 1 + 3 * 8 + 4 + 9 * k + 4 + 8;
    for (size_t i = 0; i < k; i++) l.ledger += 4 + strlen(g->cfg.labels[i]);
    l.trainer = l.ledger + LEDGER_BYTES;
    l.candidate = l.trainer + 1 + model_bytes(g);
    l.serving = l.candidate + generation_bytes(g, g->candidate);
    l.requests = l.serving + generation_bytes(g, g->serving);
    l.counters = l.requests + 8 + 4;
    for (const gateObs *o = g->oldest; o; o = o->next)
        l.counters += REQUEST_BYTES + 8 * (size_t)o->x.n;
    l.end = l.counters + COUNTERS_BYTES;
    return l;
}

/* Where the i-th waiting request starts. */
static size_t request_at(const gate *g, const layout *l, uint32_t i) {
    size_t at = l->requests + 8 + 4;

    for (const gateObs *o = g->oldest; o && i > 0; o = o->next, i--)
        at += REQUEST_BYTES + 8 * (size_t)o->x.n;
    return at;
}

/* 1 if the byte at 'at' belongs to the weights of a model: the bulk of a
 * classifier blob, and all alike. */
static int in_weights(const gate *g, const layout *l, size_t at) {
    size_t floats = model_bytes(g), from;

    if (floats == 0) return 0;
    floats -= 8;
    from = l->trainer + 1 + 8;
    if (at >= from && at < from + floats) return 1;
    from = l->candidate + 1 + GENERATION_BYTES + 8;
    if (g->candidate && at >= from && at < from + floats) return 1;
    from = l->serving + 1 + GENERATION_BYTES + 8;
    return g->serving && at >= from && at < from + floats;
}

/* The same for the regions and drift detectors of a generation. */
static int in_regions(const gate *g, const layout *l, size_t at) {
    size_t bytes = (size_t)g->nrows * (GATE_THRESHOLDS * REGION_BYTES + DRIFT_BYTES);
    size_t from = l->candidate + 1 + GENERATION_BYTES + model_bytes(g);

    if (g->candidate && at >= from && at < from + bytes) return 1;
    from = l->serving + 1 + GENERATION_BYTES + model_bytes(g);
    return g->serving && at >= from && at < from + bytes;
}

static int in_bulk(const gate *g, const layout *l, size_t at) {
    return in_weights(g, l, at) || in_regions(g, l, at);
}

/* ------------------------------ Comparison -------------------------------
 * The name of the first field in which two gates differ, "" if none.
 * Doubles and floats are compared as bits: a codec has no business turning
 * -0 into 0. */

#define DIFF(x) do { if (a->x != b->x) return #x; } while (0)
#define DIFF_BITS(x) do { \
    if (memcmp(&a->x, &b->x, sizeof(a->x)) != 0) return #x; \
} while (0)

static const char *model_diff(const gateModel *a, const gateModel *b) {
    DIFF(dim);
    DIFF(nclasses);
    DIFF(nvec);
    DIFF(updates);
    if ((a->w == NULL) != (b->w == NULL)) return "weights present";
    if (a->w == NULL) return "";
    if (memcmp(a->w, b->w, (size_t)a->nvec * a->dim * sizeof(float)) != 0) return "weights";
    if (memcmp(a->b, b->b, a->nvec * sizeof(float)) != 0) return "biases";
    return "";
}

static const char *generation_diff(const gate *g, const gateGeneration *a,
                                   const gateGeneration *b)
{
    if ((a == NULL) != (b == NULL)) return "present";
    if (a == NULL) return "";
    DIFF(gen);
    DIFF(frozen_ms);
    DIFF(promoted_ms);
    DIFF_BITS(log_threshold);
    DIFF(samples);
    DIFF(errors);
    DIFF(paired);
    DIFF(audits);
    DIFF(audit_errors);
    DIFF(budget);
    if (*model_diff(&a->model, &b->model)) return model_diff(&a->model, &b->model);
    for (uint32_t i = 0; i < g->nrows * GATE_THRESHOLDS; i++) {
        for (int j = 0; j < GATE_NBETS; j++) DIFF_BITS(regions[i].logw[j]);
        DIFF(regions[i].n);
        DIFF(regions[i].errors);
        DIFF(regions[i].certified_at);
    }
    for (uint32_t r = 0; r < g->nrows; r++) {
        DIFF(policy[r]);
        for (int k = 0; k < GATE_DRIFT_ALTS; k++) DIFF_BITS(drift[r].logw[k]);
        DIFF(drift[r].n);
        DIFF(drift[r].errors);
    }
    return "";
}

static const char *request_diff(const gateObs *a, const gateObs *b) {
    DIFF(seq);
    DIFF(created_ms);
    DIFF(flags);
    DIFF(cand_label);
    DIFF(serv_label);
    DIFF_BITS(cand_score);
    DIFF_BITS(serv_score);
    DIFF(cand_gen);
    DIFF(serv_gen);
    DIFF(result);
    DIFF(x.n);
    if (a->x.n && memcmp(a->x.idx, b->x.idx, a->x.n * sizeof(uint32_t)) != 0) return "x.idx";
    if (a->x.n && memcmp(a->x.val, b->x.val, a->x.n * sizeof(float)) != 0) return "x.val";
    return "";
}

static const char *gate_diff(const gate *a, const gate *b) {
    static char where[80];
    const gateObs *x, *y;
    const char *d;

    DIFF(cfg.kind);
    DIFF_BITS(cfg.alpha);
    DIFF_BITS(cfg.delta);
    DIFF_BITS(cfg.audit);
    DIFF(cfg.nclasses);
    DIFF(cfg.snapshot_examples);
    DIFF(cfg.feedback_timeout_ms);
    for (uint32_t i = 0; i < a->cfg.nclasses; i++) {
        if (strcmp(a->cfg.labels[i], b->cfg.labels[i]) != 0) return "cfg.labels";
        DIFF_BITS(cfg.class_alpha[i]);
        DIFF(cfg.class_defer[i]);
    }
    /* What gateAllocate() derives from the config. */
    DIFF(nrows);
    if (memcmp(a->grid, b->grid, sizeof(a->grid)) != 0) return "grid";
    if (memcmp(a->bets, b->bets, sizeof(gateBets) * a->nrows) != 0) return "bets";
    if (memcmp(a->drift_bets, b->drift_bets, sizeof(gateDriftBets) * a->nrows) != 0)
        return "drift_bets";

    DIFF(lineage);
    DIFF(epoch);
    DIFF(created_ms);
    DIFF(now_ms);
    DIFF(generations);
    DIFF(snapshot_updates);
    DIFF(paused);
    DIFF(blind);
    DIFF(missing_run);
    if (*(d = model_diff(&a->trainer, &b->trainer))) {
        snprintf(where, sizeof(where), "trainer: %s", d);
        return where;
    }
    if (*(d = generation_diff(a, a->candidate, b->candidate))) {
        snprintf(where, sizeof(where), "candidate: %s", d);
        return where;
    }
    if (*(d = generation_diff(a, a->serving, b->serving))) {
        snprintf(where, sizeof(where), "serving: %s", d);
        return where;
    }

    DIFF(next_seq);
    DIFF(npending);
    DIFF(nanswered);
    DIFF(nevidence);
    DIFF(pending_bytes);
    for (x = a->oldest, y = b->oldest; x && y; x = x->next, y = y->next) {
        if (*(d = request_diff(x, y))) {
            snprintf(where, sizeof(where), "request %llu: %s", (unsigned long long)x->seq, d);
            return where;
        }
        if (gateFindObservation(b, y->seq) != y) return "request index";
    }
    if (x || y) return "list of requests";
    /* The queue of evidence is not stored: the decoder builds it again. */
    for (x = a->ehead, y = b->ehead; x && y; x = x->enext, y = y->enext)
        if (x->seq != y->seq) return "evidence queue";
    if (x || y) return "length of the evidence queue";

    DIFF(stats.requests);
    DIFF(stats.accepted);
    DIFF(stats.audited);
    DIFF(stats.abstained);
    DIFF(stats.full);
    DIFF(stats.window);
    DIFF_BITS(stats.coverage);
    DIFF(stats.observations);
    DIFF(stats.learned);
    DIFF(stats.ignored);
    DIFF(stats.missing);
    DIFF(stats.dropped);
    DIFF(stats.promotions);
    DIFF(stats.revocations);
    DIFF(stats.retired);

    /* What a client can see. */
    if (gateState(a) != gateState(b)) return "state";
    if (gateMemoryUsage(a) != gateMemoryUsage(b)) return "memory usage";
    if (gateUnanswered(a) != gateUnanswered(b)) return "unanswered";
    if (gateFull(a) != gateFull(b)) return "full";
    for (uint32_t r = 0; r < a->nrows; r++)
        if (gateRowThreshold(a, r) != gateRowThreshold(b, r)) return "row threshold";
    return "";
}

/* ---------------------------- Simulated traffic --------------------------
 * A deterministic client and teacher that use a gate the way the module
 * does: maintain, evaluate, predict, learn. */

typedef struct sim {
    uint64_t rng;
    int64_t now;
    int wrong; /* the teacher contradicts every guess: what drift looks like */
} sim;

typedef struct asked {
    gatePredictResult res;
    int32_t answer; /* what the teacher says about this request */
} asked;

/* The clues of a request of class 'c': most of the twelve buckets of its
 * class, and a little noise that says nothing. Ascending by construction. */
static void make_clues(uint64_t *rng, uint32_t c, gateSparse *x) {
    uint32_t idx[28], n = 0;
    float val[28];

    for (uint32_t j = 0; j < 12; j++) {
        if (gate_test_rand64(rng) % 4 == 0) continue;
        idx[n] = 16 * c + j;
        val[n++] = 1.0f;
    }
    for (uint32_t j = 0; j < 16; j++) {
        if (gate_test_rand64(rng) % 8 != 0) continue;
        idx[n] = 4096 + j;
        val[n++] = gate_test_rand64(rng) % 2 ? 0.5f : -0.5f;
    }
    x->n = n;
    x->idx = gate_malloc(sizeof(uint32_t) * n);
    x->val = gate_malloc(sizeof(float) * n);
    memcpy(x->idx, idx, sizeof(uint32_t) * n);
    memcpy(x->val, val, sizeof(float) * n);
}

/* One GPREDICT at the time the clock shows. The teacher is right 97 times
 * in 100 (classifier), or agrees with every cheap result that scored at
 * least 0.5 and with half of the others (score gate). */
static asked sim_ask_now(sim *s, gate *g) {
    int classifier = g->cfg.kind == GATE_KIND_CLASSIFIER;
    gateSparse x = {0, NULL, NULL};
    double score = 0.0, u;
    gateEval ev;
    asked a;

    if (classifier) {
        uint32_t k = g->cfg.nclasses, c = (uint32_t)(gate_test_rand64(&s->rng) % k);

        make_clues(&s->rng, c, &x);
        a.answer = (int32_t)(gate_test_rand64(&s->rng) % 100 < 3 || s->wrong ? (c + 1) % k : c);
    } else {
        score = gate_test_uniform(&s->rng);
        a.answer = !s->wrong && (score >= 0.5 || gate_test_rand64(&s->rng) % 2);
    }
    u = gate_test_uniform(&s->rng);
    gateMaintain(g, s->now);
    MUST(gateEvaluate(g, classifier ? &x : NULL, score, &ev) == GATE_OK);
    MUST(gatePredict(g, s->now, u, &ev, classifier ? &x : NULL, &a.res) == GATE_OK);
    gateSparseFree(&x);
    return a;
}

/* The same a few milliseconds later. */
static asked sim_ask(sim *s, gate *g) {
    s->now += 1 + (int64_t)(gate_test_rand64(&s->rng) % 20);
    return sim_ask_now(s, g);
}

/* One request of a score gate with a chosen score and draw at a chosen
 * time. Returns its id, 0 if it was not recorded. */
static uint64_t ask_score(gate *g, int64_t now, double score, double u) {
    gatePredictResult res;
    gateEval ev;

    gateMaintain(g, now);
    MUST(gateEvaluate(g, NULL, score, &ev) == GATE_OK);
    MUST(gatePredict(g, now, u, &ev, NULL, &res) == GATE_OK);
    return res.seq;
}

static const char *const sim_labels[] = {"yes", "no", "maybe"};

/* A gate that gets through its whole life in a few hundred requests. With
 * three classes it also has a stricter and a deferred label. */
static gate *new_gate_with(int kind, uint32_t k, int64_t timeout) {
    size_t lens[3];
    gateConfig cfg;
    char err[160] = "";
    gate *g;

    gateConfigInit(&cfg);
    cfg.kind = kind;
    cfg.alpha = 0.2;
    cfg.delta = 0.2;
    cfg.audit = 0.3;
    cfg.snapshot_examples = 20;
    cfg.feedback_timeout_ms = timeout;
    for (uint32_t i = 0; i < k; i++) lens[i] = strlen(sim_labels[i]);
    if (k) MUST(gateConfigSetClasses(&cfg, sim_labels, lens, k) == GATE_OK);
    if (k == 3) {
        cfg.class_alpha[1] = 0.1;
        cfg.class_defer[2] = 1;
    }
    MUST(gateConfigValidate(&cfg, err, sizeof(err)) == GATE_OK);
    g = gateCreate(&cfg, 0x6c696e65616765ull, 0x65706f6368ull, 1700000000000ll);
    gateConfigFree(&cfg);
    return g;
}

/* Answers are lost after two seconds; proof waits half a second for one. */
#define SIM_TIMEOUT 2000
#define SIM_GRACE (SIM_TIMEOUT / GATE_GRACE_DIVISOR)

static gate *new_gate(int kind, uint32_t k) {
    return new_gate_with(kind, k, SIM_TIMEOUT);
}

/* 'n' requests, each answered at once. */
static void teach(sim *s, gate *g, int n) {
    for (int i = 0; i < n; i++) {
        asked a = sim_ask(s, g);

        if (a.res.seq) gateLearn(g, s->now, g->epoch, a.res.seq, a.answer);
    }
}

static void teach_until(sim *s, gate *g, int (*reached)(const gate *)) {
    for (int i = 0; i < 5000 && !reached(g); i++) teach(s, g, 1);
    CHECK(reached(g));
}

static int is_calibrating(const gate *g) { return g->candidate && !g->serving; }
static int is_audited(const gate *g) { return g->serving && g->serving->audits >= 5; }
static int had_alarm(const gate *g) { return g->stats.revocations > 0; }
static int has_background(const gate *g) {
    return g->serving && g->candidate && g->candidate->samples >= 5 && g->candidate->paired > 0;
}

/* Leave requests of every kind waiting: an unanswered piece of evidence,
 * two answered ones stuck behind it and, in a classifier, up to two that
 * are only kept for training, with their clues. Returns the id of the
 * unanswered piece of evidence. */
static uint64_t leave_waiting(sim *s, gate *g) {
    uint64_t head = 0;
    int behind = 0, training = 0;

    for (int i = 0; i < 100 && behind < 2; i++) {
        asked a = sim_ask(s, g);
        const gateObs *o = a.res.seq ? gateFindObservation(g, a.res.seq) : NULL;

        if (o == NULL) continue;
        if (!(o->flags & GATE_OBS_EVIDENCE)) {
            if (training++ >= 2) gateLearn(g, s->now, g->epoch, o->seq, a.answer);
        } else if (head == 0) {
            head = o->seq;
        } else {
            gateLearn(g, s->now, g->epoch, o->seq, a.answer);
            behind++;
        }
    }
    return head;
}

/* Leave a request that the proof has moved on without: the oldest piece of
 * evidence that waits (one is asked if there is none) gets no answer, the
 * time proof waits for a late answer passes, and then a newer piece of
 * evidence is answered. Returns the id of the first. */
static uint64_t leave_provisional(sim *s, gate *g) {
    uint64_t late;

    for (int i = 0; i < 200 && g->ehead == NULL; i++) {
        asked a = sim_ask(s, g);
        const gateObs *o = a.res.seq ? gateFindObservation(g, a.res.seq) : NULL;

        if (o && !(o->flags & GATE_OBS_EVIDENCE)) gateLearn(g, s->now, g->epoch, o->seq, a.answer);
    }
    if (g->ehead == NULL) return 0;
    late = g->ehead->seq;
    s->now += g->cfg.feedback_timeout_ms / GATE_GRACE_DIVISOR;
    for (int i = 0; i < 200; i++) {
        asked a = sim_ask(s, g);
        const gateObs *o = a.res.seq ? gateFindObservation(g, a.res.seq) : NULL;
        int evidence = o && (o->flags & GATE_OBS_EVIDENCE);

        if (o) gateLearn(g, s->now, g->epoch, o->seq, a.answer);
        if (evidence) break;
    }
    return late;
}

/* The feedback path breaks: 'n' requests get no answer, and none will. */
static void go_silent(sim *s, gate *g, int n) {
    for (int i = 0; i < n; i++) sim_ask(s, g);
    s->now += g->cfg.feedback_timeout_ms;
    gateMaintain(g, s->now);
}

/* 1 if a generation the request is proof for still exists (what
 * gate_core.c calls a live piece of evidence). */
static int is_live(const gate *g, const gateObs *o) {
    if ((o->flags & GATE_OBS_CAND) &&
        ((g->candidate && g->candidate->gen == o->cand_gen) ||
         (g->serving && g->serving->gen == o->cand_gen)))
        return 1;
    return (o->flags & GATE_OBS_SERV) && g->serving && g->serving->gen == o->serv_gen;
}

typedef struct waiting {
    int unanswered;  /* evidence without its answer, still in its turn */
    int answered;    /* evidence with its answer, waiting for its turn */
    int provisional; /* evidence the proof has moved on without */
    int gone;        /* ... of which for generations that are gone */
    int training;    /* requests that are not evidence */
    int clues;       /* requests that hold clues */
} waiting;

static waiting count_waiting(const gate *g) {
    waiting w = {0, 0, 0, 0, 0, 0};

    for (const gateObs *o = g->oldest; o; o = o->next) {
        if (o->flags & GATE_OBS_RESOLVED) {
            w.answered++;
        } else if (o->flags & GATE_OBS_PROVISIONAL) {
            w.provisional++;
            if (!is_live(g, o)) w.gone++;
        } else if (o->flags & GATE_OBS_EVIDENCE) {
            w.unanswered++;
        } else {
            w.training++;
        }
        if (o->x.n) w.clues++;
    }
    return w;
}

#define DRIVE_IDS 64
#define DRIVE_BLIND_OPS 24

/* A client that does a bit of everything, and the same for any two gates
 * that behave the same. Most requests get their answer at once; the others
 * later, usually oldest first, and now and then never. It also sends ids
 * that are stale or unknown, teaches (GADD), pauses for a while, stops
 * long enough for proof to move on without the answers that are late,
 * falls silent until everything has timed out, loses every answer for a
 * while (the gate goes blind and then sees again), meets a teacher that
 * turned against the gate and, rarely, resets or recovers. 'seen' is a
 * digest of every reply it got. */
typedef struct driver {
    sim s;
    uint64_t ids[DRIVE_IDS]; /* requests whose answer is still due */
    int32_t say[DRIVE_IDS];
    int n;
    int outage; /* operations left during which every answer is lost */
    uint64_t seen;
} driver;

static void driver_init(driver *d, const gate *g, uint64_t seed) {
    uint64_t answers = g->cfg.kind == GATE_KIND_CLASSIFIER ? g->cfg.nclasses : 2;

    memset(d, 0, sizeof(*d));
    d->s.rng = seed;
    d->s.now = g->now_ms;
    d->seen = seed;
    /* What waits already is this client's to answer too. */
    for (const gateObs *o = g->oldest; o && d->n < DRIVE_IDS; o = o->next) {
        if (o->flags & GATE_OBS_RESOLVED) continue;
        d->ids[d->n] = o->seq;
        d->say[d->n++] = (int32_t)(o->seq % answers);
    }
}

static void driver_step(driver *d, gate *g) {
    int classifier = g->cfg.kind == GATE_KIND_CLASSIFIER;
    uint64_t answers = classifier ? g->cfg.nclasses : 2;
    uint64_t op = gate_test_rand64(&d->s.rng) % 100, r = gate_test_rand64(&d->s.rng);
    uint64_t reply = 0;
    sim *s = &d->s;

    s->now += 1 + (int64_t)(r % 16);
    /* An outage ends with a while in which nothing asked before it can
     * still be answered: whatever the gate records then, it records blind
     * if enough answers went missing. */
    if (d->outage > 0 && --d->outage == DRIVE_BLIND_OPS) s->now += g->cfg.feedback_timeout_ms;

    if (op < 55 || (op < 85 && d->n == 0)) {
        asked a = sim_ask(s, g);

        reply = (uint64_t)a.res.action + 8 * (uint64_t)(a.res.label + 1) + 1024 * a.res.seq;
        if (d->outage) {
            /* The answer is lost. */
        } else if (a.res.seq && r % 4) {
            reply += 4 * (uint64_t)gateLearn(g, s->now, g->epoch, a.res.seq, a.answer);
        } else if (a.res.seq && d->n < DRIVE_IDS) {
            d->ids[d->n] = a.res.seq;
            d->say[d->n++] = a.answer;
        }
    } else if (op < 85) {
        /* An answer that is due; or, one time in eight and always during
         * an outage, no answer ever. */
        int j = r % 4 ? 0 : (int)((r >> 8) % (uint64_t)d->n);

        if (r % 8 && !d->outage)
            reply = (uint64_t)gateLearn(g, s->now, g->epoch, d->ids[j], d->say[j]);
        d->n--;
        memmove(&d->ids[j], &d->ids[j + 1], sizeof(d->ids[0]) * (size_t)(d->n - j));
        memmove(&d->say[j], &d->say[j + 1], sizeof(d->say[0]) * (size_t)(d->n - j));
    } else if (op < 88) {
        reply = (uint64_t)gateLearn(g, s->now, g->epoch ^ (r >> 8 & 1),
                                    r % (g->next_seq + 2), (int32_t)(r % answers));
    } else if (op < 93 && classifier) {
        uint32_t c = (uint32_t)(r % answers);
        gateSparse x;

        make_clues(&s->rng, c, &x);
        reply = gateTrain(g, s->now, &x, (int32_t)c) == GATE_OK;
        gateSparseFree(&x);
    } else if (op == 93 && r % 2 == 0) {
        /* Long enough for proof to give up waiting, not for an id to expire. */
        s->now += g->cfg.feedback_timeout_ms / GATE_GRACE_DIVISOR + (int64_t)((r >> 8) % 64);
    } else if (op == 94 && r % 8 == 0) {
        s->now += g->cfg.feedback_timeout_ms + (int64_t)(r % 1000);
    } else if (op == 95 && (g->paused || r % 8 == 0)) {
        if (g->paused) gateResume(g, s->now); else gatePause(g, s->now);
    } else if (op == 96 && (s->wrong || r % 4 == 0)) {
        s->wrong = !s->wrong;
    } else if (op == 97 && r % 64 == 0) {
        gateReset(g, s->now, r);
        d->n = 0;
    } else if (op == 97 && r % 64 == 1) {
        gateRecover(g, s->now, r);
        d->n = 0;
    } else if (op == 98 && d->outage == 0 && r % 8 == 0) {
        d->outage = DRIVE_BLIND_OPS + 60 + (int)((r >> 8) % 90);
    } else {
        gateMaintain(g, s->now);
    }
    d->seen = (d->seen ^ reply) * 0x100000001b3ull + (uint64_t)gateState(g) +
              8 * (uint64_t)(g->blind != 0);
}

static uint64_t drive(gate *g, uint64_t seed, int nops) {
    driver d;

    driver_init(&d, g, seed);
    for (int i = 0; i < nops; i++) driver_step(&d, g);
    return d.seen;
}

/* ------------------------------- Round trip ------------------------------ */

/* A digest is the plain encoding with now_ms and the seven counters of the
 * primary zeroed, and nothing else changed. */
static void check_digest_bytes(const blob *plain, const blob *digest, const layout *l) {
    size_t now = l->ledger + LEDGER_NOW, wrong = 0;

    CHECK_EQ_INT(digest->n, plain->n);
    if (digest->n != plain->n) return;
    for (size_t i = 0; i < plain->n; i++) {
        int zeroed = (i >= now && i < now + 8) || (i >= l->counters && i < l->counters + 56);

        if (digest->p[i] != (zeroed ? 0 : plain->p[i])) wrong++;
    }
    CHECK_EQ_INT(wrong, 0);
}

/* The short form of check_round_trip(), for a gate that is still needed:
 * it is consistent, its encoding has the size the layout says, it decodes
 * into the same gate, and that one encodes to the same bytes. Returns 1 if
 * all of that holds; says what does not, once per call. */
static int check_copy(const gate *g) {
    char err[160] = "";
    blob plain = encode(g, 0), again;
    const char *diff = "";
    gate *copy;
    int ok;

    ok = gateCheckInvariants(g, err, sizeof(err)) && plain.n == layout_of(g).end;
    copy = ok ? gateDecode(plain.p, plain.n, err, sizeof(err)) : NULL;
    if (copy != NULL) {
        diff = gate_diff(g, copy);
        again = encode(copy, 0);
        ok = *diff == '\0' && blob_equal(&plain, &again) &&
             gateCheckInvariants(copy, err, sizeof(err));
        blob_free(&again);
        gateFree(copy);
    } else {
        ok = 0;
    }
    if (!ok) printf("  a gate does not survive the codec: %s%s\n", err, diff);
    blob_free(&plain);
    return ok;
}

/* Everything a gate must survive, whatever state it is in. Releases 'g'. */
static void check_round_trip(gate *g, uint64_t seed) {
    char err[160] = "";
    layout l = layout_of(g);
    blob plain, digest, again, other;
    gate *copy, *third;

    CHECK(gateCheckInvariants(g, err, sizeof(err)));
    g->runtime_tag = 0x7461676765647461ull;
    g->runtime_clock = 0x636c6f636b;
    plain = encode(g, 0);
    digest = encode(g, 1);
    CHECK_EQ_INT(plain.n, l.end);
    check_digest_bytes(&plain, &digest, &l);

    copy = decode(&plain);
    CHECK(copy != NULL);
    if (copy != NULL) {
        gateCounters stats = copy->stats;
        int64_t now_ms = copy->now_ms;

        CHECK(gateCheckInvariants(copy, err, sizeof(err)));
        CHECK_EQ_STR(gate_diff(g, copy), "");
        CHECK_EQ_INT(copy->runtime_tag, 0);
        CHECK_EQ_INT(copy->runtime_clock, 0);
        again = encode(copy, 0);
        CHECK(blob_equal(&plain, &again));
        blob_free(&again);

        /* What only the primary keeps may differ without the digest
         * noticing; one more thing, and it notices. */
        copy->now_ms += 86400000;
        copy->stats.requests += 7;
        copy->stats.accepted += 3;
        copy->stats.audited += 2;
        copy->stats.abstained += 2;
        copy->stats.full += 1;
        copy->stats.window += 5;
        copy->stats.coverage = 0.015625;
        again = encode(copy, 1);
        CHECK(blob_equal(&digest, &again));
        blob_free(&again);
        again = encode(copy, 0);
        CHECK(!blob_equal(&plain, &again));
        blob_free(&again);
        copy->stats.learned++;
        again = encode(copy, 1);
        CHECK(!blob_equal(&digest, &again));
        blob_free(&again);
        copy->stats = stats;
        copy->now_ms = now_ms;

        /* The copy is the same gate: it gives the same replies from now on
         * and ends in the same state, which decodes again. */
        CHECK(drive(g, seed, 500) == drive(copy, seed, 500));
        CHECK(gateCheckInvariants(g, err, sizeof(err)));
        CHECK(gateCheckInvariants(copy, err, sizeof(err)));
        CHECK_EQ_STR(gate_diff(g, copy), "");
        again = encode(g, 0);
        other = encode(copy, 0);
        CHECK(blob_equal(&again, &other));
        CHECK(!blob_equal(&again, &plain));
        third = decode(&other);
        CHECK(third != NULL);
        gateFree(third);
        blob_free(&again);
        blob_free(&other);
        gateFree(copy);
    }
    blob_free(&plain);
    blob_free(&digest);
    gateFree(g);
}

/* ----------------------------- Reference blobs ---------------------------
 * The text form of the three blobs written by the Python reference encoder:
 * hex bytes, and '*N' for N zero bytes. */

/* Written by tests/unit/golden_codec.py: do not edit, run it again. */

#define GOLDEN_SCORE_BYTES 2338
#define GOLDEN_SCORE_NOW 73
#define GOLDEN_SCORE_COUNTERS 2218

static const char golden_score_text[] =
    "4741544501000100019a9999999999c93f7b14ae47e17a843f000000000000f0 "
    "3f00000000e80300008813000000000000080706050403020118171615141312 "
    "11feffffffffffffff0068e5cf8b01000003 *17 "
    "070000000000010300000000000000f040e5cf8b0100007854e5cf8b01000032 "
    "0000000000000002 *15 060000000000000001 *21 "
    "e03f000000000000f03f000000000000f83f0000000000000040320000000000 "
    "0000020000000000000029 *13 "
    "d0bf000000000000d03f000000000000e83f000000000000f43f1e *1677 08c0 "
    "*30 "
    "c03f000000000000d8bf06000000000000000100000000000000090000000000 "
    "00000300000005000000000000003060e5cf8b01000029000000ffffffffffff "
    "ffff *14 e83f *8 "
    "0300000000000000ffffffff0000000007000000000000001864e5cf8b010000 "
    "09000000ffffffffffffffff *14 e43f *8 "
    "0300000000000000ffffffff0000000008000000000000000c66e5cf8b010000 "
    "1b000000ffffffffffffffff *14 a03f *8 "
    "0300000000000000010000000000000064000000000000003c00000000000000 "
    "0a000000000000001e00000000000000040000000000000064 *13 "
    "e83f28000000000000001e000000000000000200000000000000010000000000 "
    "00000300000000000000010000000000000002 *15";

#define GOLDEN_CLASSIFIER_BYTES 306758
#define GOLDEN_CLASSIFIER_NOW 129
#define GOLDEN_CLASSIFIER_COUNTERS 306638

static const char golden_classifier_text[] =
    "4741544501000100009a9999999999b93f000000000000b03f000000000000d0 "
    "3f03000000040000007370616d0300000068616d0a00000068c3a96c6c6f20e2 "
    "9c939a9999999999b93f9a9999999999a93f9a9999999999b93f000001140000 "
    "00e093040000000000887766554433221100ffeeddccbbaa990068e5cf8b0100 "
    "00404ae7cf8b01000007000000000000000128 *8 1f000000012d *10 3f "
    "*32762 80be0000c03f *65530 "
    "80420000003e000080c20000004001070000000000000020b6e5cf8b01 *10 "
    "0c0000000000000003000000000000000a *23 300200000000000028 *13 80bf "
    "*65530 403f *32775 "
    "bf9a9999999999b93f9a9999999999c93f333333333333d33f9a9999999999d9 "
    "3f0c0000000000000003 *21 "
    "f83f00000000000004400000000000000c400000000000001240090000000000 "
    "0000010000000000000007 *1719 02 *3647 "
    "010500000000000000887be5cf8b010000108fe5cf8b0100005a000000000000 "
    "0004 *15 0b0000000000000002 *15 14 *32774 40 *65538 803f *8 "
    "9a9999999999b9bf9a9999999999c9bf333333333333d3bf9a9999999999d9bf "
    "0a0000000000000001 *1783 500000000000000003 *47 4f *55 4e *55 4d "
    "*55 4c *15 32 *39 4b *55 4a *15 46 *13 "
    "224000000000000023400000000000002440000000000000254049 *15 3d "
    "*3181 f43f000000000000e0bf0b0000000000000002 *39 "
    "0b00000000000000050000000200000000000000e86be5cf8b01000000000000 "
    "ffffffffffffffff *32 "
    "ffffffff0200000003000000a00f00000000803f000080bf0300000000000000 "
    "4004e6cf8b010000250000000000000002 *9 "
    "e43f000000000000e13f07000000000000000500000000000000ffffffff0200 "
    "000007000000fe1f00000000003f0000c03f0500000000000000b015e7cf8b01 "
    "00000d00000001 *13 "
    "e83f000000000000e03f07000000000000000500000000000000ffffffff0300 "
    "00000500000064000000ff1f00000000803f000000bf0000803e060000000000 "
    "0000b115e7cf8b0100001f0000000000000001 *9 "
    "ec3f000000000000ee3f07000000000000000500000000000000020000000000 "
    "00000900000000000000404ae7cf8b010000020000000200000001 *9 "
    "dc3f000000000000ef3f07000000000000000500000000000000ffffffff01 *10 "
    "c0e803000000000000bc020000000000006400000000000000c8000000000000 "
    "0007 *8 02 *12 "
    "e03f2c01000000000000fa000000000000000400000000000000050000000000 "
    "0000060000000000000002000000000000000100000000000000030000000000 "
    "0000";

#define GOLDEN_BLIND_BYTES 442
#define GOLDEN_BLIND_NOW 73
#define GOLDEN_BLIND_COUNTERS 322

static const char golden_blind_text[] =
    "4741544501000100019a9999999999a93f9a9999999999b93f000000000000e0 "
    "3f000000000700000060ea000000000000a8a7a6a5a4a3a2a1b8b7b6b5b4b3b2 "
    "b101 *8 5c26050000000004 *16 "
    "01000000000000008967452301000000030000000060452301000000b0982505 "
    "0000000024000000ffffffffffffffff000000000000ee3f *8 04 *15 "
    "ffffffff000000000067452301000000185826050000000001000000ffffffff "
    "ffffffff *32 ffffffff000000008867452301000000005c2605 *8 "
    "ffffffffffffffff *32 "
    "ffffffff00000000840300000000000064000000000000003200000000000000 "
    "ee02 *30 bc02000000000000f401 *14 "
    "60000000000000000c00000000000000030000000000000003 *15";

/* What the blobs do not store. Score gate: one tested row, generation 3,
 * DELTA 0.01; it answers from its lowest threshold, grid[0] = 0. Classifier:
 * two tested rows, generations 7 (tested) and 5 (answering), DELTA 0.0625;
 * row 1 answers from grid[4], with the grid running from 1/3 to 0.999. */
#define GOLDEN_SCORE_BAR 10.555812738575817
#define GOLDEN_CLASSIFIER_CANDIDATE_BAR 10.956823496334604
#define GOLDEN_CLASSIFIER_SERVING_BAR 10.332669187261608
#define GOLDEN_CLASSIFIER_THRESHOLD 0.7119065267033905

/* End of what golden_codec.py writes. */

static size_t golden_expand(const char *s, unsigned char *out) {
    size_t n = 0;

    while (*s) {
        if (*s == ' ') {
            s++;
        } else if (*s == '*') {
            char *end;
            size_t run = strtoul(s + 1, &end, 10);

            if (out) memset(out + n, 0, run);
            n += run;
            s = end;
        } else {
            unsigned int byte = 0;

            sscanf(s, "%2x", &byte);
            if (out) out[n] = (unsigned char)byte;
            n++;
            s += 2;
        }
    }
    return n;
}

static blob golden(const char *text) {
    blob b;

    b.n = golden_expand(text, NULL);
    b.p = gate_malloc(b.n);
    golden_expand(text, b.p);
    return b;
}

static void set_clues(gateSparse *x, uint32_t n, const uint32_t *idx, const float *val) {
    x->n = n;
    x->idx = gate_malloc(sizeof(uint32_t) * n);
    x->val = gate_malloc(sizeof(float) * n);
    memcpy(x->idx, idx, sizeof(uint32_t) * n);
    memcpy(x->val, val, sizeof(float) * n);
}

/* A request of a gate built by hand: the caller fills in the rest and
 * links it. */
static gateObs *new_request(uint64_t seq, int64_t created_ms, uint32_t flags) {
    gateObs *o = gateObsAllocate();

    o->seq = seq;
    o->created_ms = created_ms;
    o->flags = flags;
    return o;
}

/* The score gate of the first reference blob. It answers (generation 3
 * serves, its widest region is proven), nothing is being tested, and three
 * spot checks wait: one the proof has moved on without, one for its
 * answer, and one behind that with its answer. The policy is set by hand:
 * the decoder must find the same. */
static gate *golden_score_gate(void) {
    gateConfig cfg;
    char err[160] = "";
    gateGeneration *s;
    gateObs *o;
    gate *g;

    gateConfigInit(&cfg);
    cfg.alpha = 0.2;
    cfg.delta = 0.01;
    cfg.audit = 1.0;
    cfg.snapshot_examples = 1000;
    cfg.feedback_timeout_ms = 5000;
    MUST(gateConfigValidate(&cfg, err, sizeof(err)) == GATE_OK);
    g = gateAllocate(&cfg);
    g->lineage = 0x0102030405060708ull;
    g->epoch = 0x1112131415161718ull;
    g->created_ms = -2;
    g->now_ms = 1700000000000ll;
    g->generations = 3;
    g->missing_run = 7;

    s = g->serving = gateGenerationAllocate(g);
    s->gen = 3;
    s->frozen_ms = 1699999990000ll;
    s->promoted_ms = 1699999995000ll;
    s->samples = 50;
    s->errors = 2;
    s->audits = 6;
    s->audit_errors = 1;
    s->regions[0] = (gateRegion){{0.5, 1.0, 1.5, 2.0}, 50, 2, 41};
    s->regions[1] = (gateRegion){{-0.25, 0.25, 0.75, 1.25}, 30, 0, 0};
    s->regions[31].logw[3] = -3.0;
    s->drift[0] = (gateDrift){{0.125, -0.375}, 6, 1};
    s->policy[0] = 0;
    s->log_threshold = gateBar(g, 3);

    g->next_seq = 9;
    o = new_request(5, 1699999998000ll, GATE_OBS_AUDIT | GATE_OBS_SERV | GATE_OBS_PROVISIONAL);
    o->serv_score = 0.75;
    o->serv_gen = 3;
    gateObsLink(g, o);
    o = new_request(7, 1699999999000ll, GATE_OBS_AUDIT | GATE_OBS_SERV);
    o->serv_score = 0.625;
    o->serv_gen = 3;
    gateObsLink(g, o);
    o = new_request(8, 1699999999500ll,
                    GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_SERV | GATE_OBS_RESOLVED);
    o->serv_score = 0.03125;
    o->serv_gen = 3;
    o->result = 1;
    gateObsLink(g, o);

    g->stats = (gateCounters){
        .requests = 100, .accepted = 60, .audited = 10, .abstained = 30, .full = 4,
        .window = 100, .coverage = 0.75, .observations = 40, .learned = 30, .ignored = 2,
        .missing = 1, .dropped = 3, .promotions = 1, .revocations = 2, .retired = 0,
    };
    return g;
}

/* The classifier of the second reference blob: three labels (one with a
 * stricter error rate, one deferred), paused, generation 5 serving and
 * generation 7 being tested, and five waiting requests: one kept for
 * training from before any snapshot, one the proof has moved on without,
 * an unanswered piece of evidence, an answered one behind it, and one
 * recorded while paused. Without 'requests' nothing waits. */
static gate *golden_classifier_gate(int requests) {
    static const char *const labels[] = {"spam", "ham", "h\xc3\xa9llo \xe2\x9c\x93"};
    static const size_t lens[] = {4, 3, 10};
    static const uint32_t idx2[] = {3, 4000}, idx3[] = {7, 8190}, idx5[] = {5, 100, 8191};
    static const uint32_t idx9[] = {0};
    static const float val2[] = {1.0f, -1.0f}, val3[] = {0.5f, 1.5f};
    static const float val5[] = {1.0f, -0.5f, 0.25f}, val9[] = {-2.0f};
    gateConfig cfg;
    char err[160] = "";
    gateGeneration *c, *s;
    gateObs *o;
    gate *g;

    gateConfigInit(&cfg);
    cfg.kind = GATE_KIND_CLASSIFIER;
    cfg.alpha = 0.1;
    cfg.delta = 0.0625;
    cfg.audit = 0.25;
    cfg.snapshot_examples = 20;
    cfg.feedback_timeout_ms = 300000;
    MUST(gateConfigSetClasses(&cfg, labels, lens, 3) == GATE_OK);
    cfg.class_alpha[1] = 0.05;
    cfg.class_defer[2] = 1;
    MUST(gateConfigValidate(&cfg, err, sizeof(err)) == GATE_OK);
    g = gateAllocate(&cfg);
    gateConfigFree(&cfg);
    g->lineage = 0x1122334455667788ull;
    g->epoch = 0x99aabbccddeeff00ull;
    g->created_ms = 1700000000000ll;
    g->now_ms = 1700000123456ll;
    g->generations = 7;
    g->paused = 1;
    g->snapshot_updates = 40;
    g->missing_run = 31;

    g->trainer.updates = 45;
    g->trainer.w[0] = 0.5f;
    g->trainer.w[8191] = -0.25f;
    g->trainer.w[8192] = 1.5f;
    g->trainer.w[24575] = 64.0f;
    g->trainer.b[0] = 0.125f;
    g->trainer.b[1] = -64.0f;
    g->trainer.b[2] = 2.0f;

    c = g->candidate = gateGenerationAllocate(g);
    c->gen = 7;
    c->frozen_ms = 1700000020000ll;
    c->samples = 12;
    c->errors = 3;
    c->paired = 10;
    c->budget = 560;
    MUST(gateModelInit(&c->model, GATE_DIM, 3) == GATE_OK);
    c->model.updates = 40;
    c->model.w[1] = -1.0f;
    c->model.w[16384] = 0.75f;
    c->model.b[2] = -0.5f;
    c->regions[0] = (gateRegion){{0.1, 0.2, 0.3, 0.4}, 12, 3, 0};
    c->regions[1] = (gateRegion){{1.5, 2.5, 3.5, 4.5}, 9, 1, 7};
    c->regions[32].n = 2;
    c->policy[0] = 1;
    c->log_threshold = gateBar(g, 7);

    s = g->serving = gateGenerationAllocate(g);
    s->gen = 5;
    s->frozen_ms = 1700000005000ll;
    s->promoted_ms = 1700000010000ll;
    s->samples = 90;
    s->errors = 4;
    s->audits = 11;
    s->audit_errors = 2;
    MUST(gateModelInit(&s->model, GATE_DIM, 3) == GATE_OK);
    s->model.updates = 20;
    s->model.w[8191] = 2.0f;
    s->model.b[0] = 1.0f;
    s->regions[0] = (gateRegion){{-0.1, -0.2, -0.3, -0.4}, 10, 1, 0};
    for (uint32_t t = 0; t < 8; t++) s->regions[32 + t].n = 80 - t;
    s->regions[32 + 0].errors = 3;
    s->regions[32 + 4].certified_at = 50;
    s->regions[32 + 6].certified_at = 70;
    s->regions[32 + 7] = (gateRegion){{9.0, 9.5, 10.0, 10.5}, 73, 0, 61};
    s->drift[1] = (gateDrift){{1.25, -0.5}, 11, 2};
    s->policy[1] = 4;
    s->log_threshold = gateBar(g, 5);

    g->next_seq = 11;
    if (requests) {
        o = new_request(2, 1700000001000ll, 0);
        set_clues(&o->x, 2, idx2, val2);
        gateObsLink(g, o);
        o = new_request(3, 1700000040000ll, GATE_OBS_AUDIT | GATE_OBS_CAND | GATE_OBS_PROVISIONAL);
        o->cand_label = 0;
        o->serv_label = 2;
        o->cand_score = 0.625;
        o->serv_score = 0.53125;
        o->cand_gen = 7;
        o->serv_gen = 5;
        set_clues(&o->x, 2, idx3, val3);
        gateObsLink(g, o);
        o = new_request(5, 1700000110000ll, GATE_OBS_AUDIT | GATE_OBS_CAND | GATE_OBS_SERV);
        o->cand_label = 1;
        o->serv_label = 0;
        o->cand_score = 0.75;
        o->serv_score = 0.5;
        o->cand_gen = 7;
        o->serv_gen = 5;
        set_clues(&o->x, 3, idx5, val5);
        gateObsLink(g, o);
        o = new_request(6, 1700000110001ll, GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT |
                                            GATE_OBS_EVIDENCE | GATE_OBS_RESOLVED);
        o->cand_label = 0;
        o->serv_label = 1;
        o->cand_score = 0.875;
        o->serv_score = 0.9375;
        o->cand_gen = 7;
        o->serv_gen = 5;
        o->result = 2;
        gateObsLink(g, o);
        o = new_request(9, 1700000123456ll, GATE_OBS_WOULD_ACCEPT);
        o->cand_label = 2;
        o->serv_label = 1;
        o->cand_score = 0.4375;
        o->serv_score = 0.96875;
        o->cand_gen = 7;
        o->serv_gen = 5;
        set_clues(&o->x, 1, idx9, val9);
        gateObsLink(g, o);
    }

    g->stats = (gateCounters){
        .requests = 1000, .accepted = 700, .audited = 100, .abstained = 200, .full = 7,
        .window = 512, .coverage = 0.5, .observations = 300, .learned = 250, .ignored = 4,
        .missing = 5, .dropped = 6, .promotions = 2, .revocations = 1, .retired = 3,
    };
    return g;
}

/* The gate of the third reference blob: a score gate whose answers stopped
 * arriving. It holds no generation. Three requests wait: one the proof had
 * moved on without before the generation it was proof for was dropped, and
 * two recorded while blind so that there is an id to answer. */
static gate *golden_blind_gate(void) {
    gateConfig cfg;
    char err[160] = "";
    gateObs *o;
    gate *g;

    gateConfigInit(&cfg);
    cfg.alpha = 0.05;
    cfg.delta = 0.1;
    cfg.audit = 0.5;
    cfg.snapshot_examples = 7;
    cfg.feedback_timeout_ms = 60000;
    MUST(gateConfigValidate(&cfg, err, sizeof(err)) == GATE_OK);
    g = gateAllocate(&cfg);
    g->lineage = 0xa1a2a3a4a5a6a7a8ull;
    g->epoch = 0xb1b2b3b4b5b6b7b8ull;
    g->created_ms = 1;
    g->now_ms = 86400000;
    g->generations = 4;
    g->blind = 1;

    g->next_seq = 0x123456789ull;
    o = new_request(0x123456000ull, 86350000, GATE_OBS_CAND | GATE_OBS_PROVISIONAL);
    o->cand_score = 0.9375;
    o->cand_gen = 4;
    gateObsLink(g, o);
    gateObsLink(g, new_request(0x123456700ull, 86399000, GATE_OBS_AUDIT));
    gateObsLink(g, new_request(0x123456788ull, 86400000, 0));

    g->stats = (gateCounters){
        .requests = 900, .accepted = 100, .audited = 50, .abstained = 750, .full = 0,
        .window = 0, .coverage = 0.0, .observations = 700, .learned = 500, .ignored = 0,
        .missing = 96, .dropped = 12, .promotions = 3, .revocations = 3, .retired = 0,
    };
    return g;
}

/* The first byte that differs, for a failure message one can act on. */
static void check_same_bytes(const blob *got, const blob *want) {
    size_t i = 0;

    CHECK_EQ_INT(got->n, want->n);
    while (i < got->n && i < want->n && got->p[i] == want->p[i]) i++;
    if (i < got->n && i < want->n)
        printf("  first difference at byte %zu: %02x, expected %02x\n", i, got->p[i], want->p[i]);
    CHECK(blob_equal(got, want));
}

/* A gate built by hand and the blob the Python script wrote for it: the
 * gate encodes to the blob, the blob decodes to the gate and encodes to
 * itself, and the layout computed here agrees with the offsets the script
 * reports. Returns the decoded gate (NULL if there is none) for a look at
 * its fields by name. */
static gate *check_reference(const gate *g, const char *text, size_t bytes, size_t now,
                             size_t counters)
{
    blob want = golden(text), got = encode(g, 0), again;
    layout l = layout_of(g);
    char err[160] = "";
    gate *d;

    CHECK(gateCheckInvariants(g, err, sizeof(err)));
    CHECK_EQ_INT(want.n, bytes);
    CHECK_EQ_INT(l.ledger + LEDGER_NOW, now);
    CHECK_EQ_INT(l.counters, counters);
    CHECK_EQ_INT(l.end, want.n);
    check_same_bytes(&got, &want);

    d = decode(&want);
    CHECK(d != NULL);
    if (d != NULL) {
        CHECK(gateCheckInvariants(d, err, sizeof(err)));
        CHECK_EQ_STR(gate_diff(g, d), "");
        again = encode(d, 0);
        check_same_bytes(&again, &want);
        blob_free(&again);
    }
    blob_free(&got);
    blob_free(&want);
    return d;
}

TEST(reference_score_gate) {
    gate *g = golden_score_gate();
    gate *d = check_reference(g, golden_score_text, GOLDEN_SCORE_BYTES, GOLDEN_SCORE_NOW,
                              GOLDEN_SCORE_COUNTERS);

    /* 8 + config 41 + ledger 54 + three presence bytes + a generation of
     * 72 + 32 * 56 + 32 + 12 + three requests of 68 + counters 120. */
    CHECK_EQ_INT(GOLDEN_SCORE_BYTES, 2338);
    if (d != NULL) {
        const gateObs *late = gateFindObservation(d, 5), *head = gateFindObservation(d, 7);

        /* A few of the fields by name, in case both the builder above and
         * the decoder got one wrong in the same way. */
        CHECK_EQ_INT(d->cfg.kind, GATE_KIND_SCORE);
        CHECK(d->cfg.alpha == 0.2 && d->cfg.delta == 0.01 && d->cfg.audit == 1.0);
        CHECK_EQ_INT(d->cfg.feedback_timeout_ms, 5000);
        CHECK(d->lineage == 0x0102030405060708ull && d->epoch == 0x1112131415161718ull);
        CHECK_EQ_INT(d->created_ms, -2);
        CHECK_EQ_INT(d->now_ms, 1700000000000ll);
        CHECK_EQ_INT(d->blind, 0);
        CHECK_EQ_INT(d->missing_run, 7);
        CHECK(d->candidate == NULL && d->trainer.w == NULL);
        CHECK_EQ_INT(d->serving->regions[0].certified_at, 41);
        CHECK(d->serving->regions[31].logw[3] == -3.0);
        CHECK(d->serving->drift[0].logw[1] == -0.375);
        CHECK_EQ_INT(d->npending, 3);
        CHECK_EQ_INT(d->pending_bytes, 3 * sizeof(gateObs));
        CHECK_EQ_INT(d->stats.revocations, 2);
        CHECK(d->stats.coverage == 0.75);

        /* What is not stored. The queue of evidence: the request that
         * still waits for its answer and the answered one behind it, not
         * the one the proof has moved on without. */
        CHECK_EQ_INT(d->nevidence, 2);
        CHECK_EQ_INT(d->nanswered, 1);
        CHECK_EQ_INT(gateUnanswered(d), 2);
        CHECK(late != NULL && late == d->oldest && late->enext == NULL);
        CHECK(late && late->flags == (GATE_OBS_AUDIT | GATE_OBS_SERV | GATE_OBS_PROVISIONAL));
        CHECK(late && late->serv_score == 0.75 && late->result == -1);
        CHECK(head != NULL && head == d->ehead && head->enext == d->etail);
        CHECK(gateFindObservation(d, 8) == d->newest && d->newest == d->etail);
        CHECK_EQ_INT(d->newest->result, 1);
        CHECK(d->newest->serv_score == 0.03125);
        /* The bar is log(3 * 4) + log(1 * 32) - log(0.01); the widest
         * region is proven, so the gate answers from score 0 up. */
        CHECK_NEAR(d->serving->log_threshold, GOLDEN_SCORE_BAR, 1e-12);
        CHECK_NEAR(GOLDEN_SCORE_BAR, 10.5558127, 1e-7);
        CHECK_EQ_INT(d->serving->policy[0], 0);
        CHECK(gateRowThreshold(d, 0) == 0.0);
        CHECK(gateState(d) == GATE_STATE_ACTIVE);
        gateFree(d);
    }
    gateFree(g);
}

TEST(reference_classifier_gate) {
    gate *g = golden_classifier_gate(1);
    gate *d = check_reference(g, golden_classifier_text, GOLDEN_CLASSIFIER_BYTES,
                              GOLDEN_CLASSIFIER_NOW, GOLDEN_CLASSIFIER_COUNTERS);

    /* 8 + config 97 + ledger 54 + trainer 1 + 98324 + two generations of
     * 1 + 72 + 98324 + 3 * 1824 + 12 + five requests of 68 with eight
     * clues of 8 + counters 120, a model being 8 + 3 * 8192 * 4 + 3 * 4. */
    CHECK_EQ_INT(GOLDEN_CLASSIFIER_BYTES, 306758);
    if (d != NULL) {
        const gateObs *late = gateFindObservation(d, 3), *head = gateFindObservation(d, 5);

        CHECK_EQ_INT(d->cfg.nclasses, 3);
        CHECK_EQ_STR(d->cfg.labels[0], "spam");
        CHECK_EQ_STR(d->cfg.labels[1], "ham");
        CHECK_EQ_STR(d->cfg.labels[2], "h\xc3\xa9llo \xe2\x9c\x93");
        CHECK(d->cfg.alpha == 0.1 && d->cfg.delta == 0.0625 && d->cfg.audit == 0.25);
        CHECK(d->cfg.class_alpha[0] == 0.1 && d->cfg.class_alpha[1] == 0.05);
        CHECK(!d->cfg.class_defer[0] && !d->cfg.class_defer[1] && d->cfg.class_defer[2]);
        CHECK_EQ_INT(d->cfg.feedback_timeout_ms, 300000);
        CHECK_EQ_INT(d->paused, 1);
        CHECK_EQ_INT(d->snapshot_updates, 40);
        CHECK_EQ_INT(d->blind, 0);
        CHECK_EQ_INT(d->missing_run, 31);
        CHECK_EQ_INT(d->trainer.updates, 45);
        CHECK(d->trainer.w[8191] == -0.25f && d->trainer.w[8192] == 1.5f);
        CHECK(d->trainer.w[24575] == 64.0f && d->trainer.b[1] == -64.0f);
        CHECK(d->candidate->model.w[16384] == 0.75f && d->candidate->model.b[2] == -0.5f);
        CHECK(d->serving->model.w[8191] == 2.0f && d->serving->model.b[0] == 1.0f);
        CHECK_EQ_INT(d->candidate->budget, 560);
        CHECK_EQ_INT(d->candidate->paired, 10);
        CHECK_EQ_INT(d->serving->regions[32 + 7].n, 73);
        CHECK(d->serving->drift[1].logw[0] == 1.25);
        CHECK_EQ_INT(d->npending, 5);
        CHECK_EQ_INT(d->pending_bytes, 5 * sizeof(gateObs) + 8 * 8);
        CHECK_EQ_INT(d->newest->seq, 9);
        CHECK(d->newest->x.val[0] == -2.0f);

        /* What is not stored. The queue of evidence is the unanswered
         * request and the answered one behind it; the request the proof
         * has moved on without keeps its clues for the day its label
         * arrives, and is not in the queue. */
        CHECK_EQ_INT(d->nevidence, 2);
        CHECK_EQ_INT(d->nanswered, 1);
        CHECK_EQ_INT(gateUnanswered(d), 4);
        CHECK(head != NULL && head == d->ehead && head->x.n == 3);
        CHECK(head && head->x.idx[2] == 8191 && head->x.val[1] == -0.5f);
        CHECK(head && head->cand_score == 0.75 && head->serv_score == 0.5);
        CHECK(head && head->enext == d->etail && d->etail->seq == 6 && d->etail->result == 2);
        CHECK(late != NULL && late->enext == NULL && late->next == head);
        CHECK(late && late->flags == (GATE_OBS_AUDIT | GATE_OBS_CAND | GATE_OBS_PROVISIONAL));
        CHECK(late && late->x.n == 2 && late->x.idx[1] == 8190 && late->x.val[1] == 1.5f);
        CHECK(late && late->cand_score == 0.625 && late->serv_score == 0.53125);
        CHECK(late && late->cand_label == 0 && late->serv_label == 2 && late->result == -1);
        /* Two tested rows of 32 thresholds, each a test of its own: bar =
         * log(g (g + 1)) + log(64) - log(0.0625). In row 1 of the serving
         * generation regions 4, 6 and 7 are proven, so the row answers
         * from the lowest of them: threshold 4, which is
         * 1 - (2/3) * (0.001 / (2/3)) ^ (4/31). The candidate has proven
         * region 1 of row 0. */
        CHECK_NEAR(d->candidate->log_threshold, GOLDEN_CLASSIFIER_CANDIDATE_BAR, 1e-12);
        CHECK_NEAR(d->serving->log_threshold, GOLDEN_CLASSIFIER_SERVING_BAR, 1e-12);
        CHECK_NEAR(GOLDEN_CLASSIFIER_CANDIDATE_BAR, 10.9568235, 1e-7);
        CHECK_NEAR(GOLDEN_CLASSIFIER_SERVING_BAR, 10.3326692, 1e-7);
        CHECK_EQ_INT(d->serving->policy[0], -1);
        CHECK_EQ_INT(d->serving->policy[1], 4);
        CHECK_EQ_INT(d->serving->policy[2], -1);
        CHECK_EQ_INT(d->candidate->policy[0], 1);
        CHECK_EQ_INT(d->candidate->policy[1], -1);
        CHECK_EQ_INT(d->candidate->policy[2], -1);
        CHECK_NEAR(gateRowThreshold(d, 1), GOLDEN_CLASSIFIER_THRESHOLD, 1e-14);
        CHECK_NEAR(GOLDEN_CLASSIFIER_THRESHOLD, 0.71190653, 1e-8);
        CHECK(gateRowThreshold(d, 0) < 0 && gateRowThreshold(d, 2) < 0);
        CHECK(gateState(d) == GATE_STATE_PAUSED);
        gateFree(d);
    }
    gateFree(g);
}

TEST(reference_blind_gate) {
    gate *g = golden_blind_gate();
    gate *d = check_reference(g, golden_blind_text, GOLDEN_BLIND_BYTES, GOLDEN_BLIND_NOW,
                              GOLDEN_BLIND_COUNTERS);

    /* 8 + config 41 + ledger 54 + three presence bytes + 12 + three
     * requests of 68 + counters 120. */
    CHECK_EQ_INT(GOLDEN_BLIND_BYTES, 442);
    if (d != NULL) {
        const gateObs *late = d->oldest;

        CHECK_EQ_INT(d->cfg.kind, GATE_KIND_SCORE);
        CHECK(d->cfg.alpha == 0.05 && d->cfg.delta == 0.1 && d->cfg.audit == 0.5);
        CHECK_EQ_INT(d->cfg.snapshot_examples, 7);
        CHECK_EQ_INT(d->cfg.feedback_timeout_ms, 60000);
        CHECK(d->lineage == 0xa1a2a3a4a5a6a7a8ull && d->epoch == 0xb1b2b3b4b5b6b7b8ull);
        CHECK_EQ_INT(d->created_ms, 1);
        CHECK_EQ_INT(d->now_ms, 86400000);
        CHECK_EQ_INT(d->generations, 4);
        CHECK_EQ_INT(d->blind, 1);
        CHECK_EQ_INT(d->missing_run, 0);
        CHECK_EQ_INT(d->paused, 0);
        CHECK(d->candidate == NULL && d->serving == NULL && d->trainer.w == NULL);
        CHECK(d->next_seq == 0x123456789ull);
        CHECK_EQ_INT(d->npending, 3);
        CHECK(late && late->seq == 0x123456000ull && late->cand_gen == 4);
        CHECK(late && late->flags == (GATE_OBS_CAND | GATE_OBS_PROVISIONAL));
        CHECK(late && late->cand_score == 0.9375 && late->created_ms == 86350000);
        CHECK(late && late->next && late->next->flags == GATE_OBS_AUDIT);
        CHECK(d->newest->seq == 0x123456788ull && d->newest->flags == 0);
        CHECK_EQ_INT(d->stats.missing, 96);
        CHECK_EQ_INT(d->stats.dropped, 12);

        /* What is not stored: nothing waits for its turn, and a gate that
         * holds no generation is COLD and answers nothing. */
        CHECK(d->ehead == NULL && d->etail == NULL);
        CHECK_EQ_INT(d->nevidence, 0);
        CHECK_EQ_INT(d->nanswered, 0);
        CHECK_EQ_INT(gateUnanswered(d), 3);
        CHECK(gateRowThreshold(d, 0) < 0);
        CHECK(gateState(d) == GATE_STATE_COLD);
        gateFree(d);
    }
    gateFree(g);
}

/* The digests of the three reference gates are the reference blobs with
 * the primary-only fields zeroed, at the offsets the Python script reports. */
TEST(reference_digests) {
    static const struct {
        const char *text;
        size_t now, counters;
    } ref[] = {
        {golden_score_text, GOLDEN_SCORE_NOW, GOLDEN_SCORE_COUNTERS},
        {golden_classifier_text, GOLDEN_CLASSIFIER_NOW, GOLDEN_CLASSIFIER_COUNTERS},
        {golden_blind_text, GOLDEN_BLIND_NOW, GOLDEN_BLIND_COUNTERS},
    };

    for (int i = 0; i < 3; i++) {
        gate *g = i == 0 ? golden_score_gate() : i == 1 ? golden_classifier_gate(1)
                                                        : golden_blind_gate();
        blob want = golden(ref[i].text), got = encode(g, 1);

        memset(want.p + ref[i].now, 0, 8);
        memset(want.p + ref[i].counters, 0, 7 * 8);
        check_same_bytes(&got, &want);
        blob_free(&got);
        blob_free(&want);
        gateFree(g);
    }
}

/* Each field on its own: a digest ignores exactly the eight that only the
 * primary maintains. The plain encoding ignores none. */
#define DIGEST_CASE(base, ignored, ...) do { \
    gate *g = clone(base); \
    blob _d0 = encode(base, 1), _p0 = encode(base, 0), _d, _p; \
    CHECK(g != NULL); \
    if (g != NULL) { \
        __VA_ARGS__; \
        _d = encode(g, 1); \
        _p = encode(g, 0); \
        CHECK(blob_equal(&_d0, &_d) == (ignored)); \
        CHECK(!blob_equal(&_p0, &_p)); \
        blob_free(&_d); \
        blob_free(&_p); \
        gateFree(g); \
    } \
    blob_free(&_d0); \
    blob_free(&_p0); \
} while (0)

TEST(digest_fields) {
    gate *ref = golden_classifier_gate(1), *blind = golden_blind_gate();

    DIGEST_CASE(ref, 1, g->now_ms++);
    DIGEST_CASE(ref, 1, g->stats.requests++);
    DIGEST_CASE(ref, 1, g->stats.accepted++);
    DIGEST_CASE(ref, 1, g->stats.audited++);
    DIGEST_CASE(ref, 1, g->stats.abstained++);
    DIGEST_CASE(ref, 1, g->stats.full++);
    DIGEST_CASE(ref, 1, g->stats.window++);
    DIGEST_CASE(ref, 1, g->stats.coverage = 0.25);

    DIGEST_CASE(ref, 0, g->stats.observations++);
    DIGEST_CASE(ref, 0, g->stats.learned++);
    DIGEST_CASE(ref, 0, g->stats.ignored++);
    DIGEST_CASE(ref, 0, g->stats.missing++);
    DIGEST_CASE(ref, 0, g->stats.dropped++);
    DIGEST_CASE(ref, 0, g->stats.promotions++);
    DIGEST_CASE(ref, 0, g->stats.revocations++);
    DIGEST_CASE(ref, 0, g->stats.retired++);
    DIGEST_CASE(ref, 0, g->lineage++);
    DIGEST_CASE(ref, 0, g->epoch++);
    DIGEST_CASE(ref, 0, g->created_ms++);
    DIGEST_CASE(ref, 0, g->generations++);
    DIGEST_CASE(ref, 0, g->paused = 0);
    DIGEST_CASE(ref, 0, g->snapshot_updates++);
    DIGEST_CASE(ref, 0, g->next_seq++);
    DIGEST_CASE(ref, 0, g->cfg.audit = 0.5);
    DIGEST_CASE(ref, 0, g->cfg.feedback_timeout_ms++);
    DIGEST_CASE(ref, 0, g->cfg.class_defer[0] = 1);
    DIGEST_CASE(ref, 0, g->trainer.updates++);
    DIGEST_CASE(ref, 0, g->trainer.w[12345] = 0.5f);
    DIGEST_CASE(ref, 0, g->candidate->samples++);
    DIGEST_CASE(ref, 0, g->candidate->model.b[0] = 0.5f);
    DIGEST_CASE(ref, 0, g->candidate->regions[95].logw[3] = 1.0);
    DIGEST_CASE(ref, 0, g->serving->promoted_ms++);
    DIGEST_CASE(ref, 0, g->serving->audits++);
    DIGEST_CASE(ref, 0, g->serving->drift[2].errors++);
    DIGEST_CASE(ref, 0, g->oldest->created_ms++);
    DIGEST_CASE(ref, 0, g->oldest->x.val[1] = 3.0f);
    DIGEST_CASE(ref, 0, g->newest->flags |= GATE_OBS_AUDIT);
    DIGEST_CASE(ref, 0, g->newest->serv_score = 0.5);

    /* What the gate knows about late and lost answers is replicated: a
     * replica passes the same requests by and goes blind with its primary. */
    DIGEST_CASE(ref, 0, g->missing_run = 30);
    DIGEST_CASE(ref, 0, gateFindObservation(g, 3)->flags &= ~(uint32_t)GATE_OBS_PROVISIONAL);
    DIGEST_CASE(ref, 0, gateFindObservation(g, 5)->flags |= GATE_OBS_PROVISIONAL);
    DIGEST_CASE(blind, 0, g->blind = 0);
    DIGEST_CASE(blind, 0, g->missing_run = 1);
    DIGEST_CASE(blind, 1, g->now_ms++);
    DIGEST_CASE(blind, 1, g->stats.abstained++; g->stats.requests++);
    gateFree(ref);
    gateFree(blind);
}

/* runtime_tag and runtime_clock belong to the embedding layer: they are
 * neither encoded, not even in a digest, nor kept (check_round_trip()
 * checks that a copy starts with 0). */
TEST(runtime_fields_are_not_encoded) {
    gate *g = golden_score_gate();
    blob a = encode(g, 0), d = encode(g, 1), b;

    g->runtime_tag = ~0ull;
    g->runtime_clock = INT64_MIN;
    b = encode(g, 0);
    CHECK(blob_equal(&a, &b));
    blob_free(&b);
    b = encode(g, 1);
    CHECK(blob_equal(&d, &b));
    blob_free(&a);
    blob_free(&b);
    blob_free(&d);
    gateFree(g);
}

/* --------------------------------- States --------------------------------
 * Gates brought to each state of their life by the simulated traffic. */

/* A score gate with requests waiting: still being tested, or answering. */
static gate *score_with_requests(uint64_t seed, int active) {
    gate *g = new_gate(GATE_KIND_SCORE, 0);
    sim s = {seed, g->now_ms, 0};

    if (active) teach_until(&s, g, is_audited); else teach(&s, g, 25);
    leave_waiting(&s, g);
    return g;
}

/* A classifier that has frozen nothing yet: five requests wait with their
 * clues, to train the model when their labels arrive. */
static gate *classifier_learning(uint64_t seed) {
    gate *g = new_gate(GATE_KIND_CLASSIFIER, 2);
    sim s = {seed, g->now_ms, 0};

    teach(&s, g, 8);
    for (int i = 0; i < 5; i++) sim_ask(&s, g);
    return g;
}

/* A classifier that answers while a newer snapshot collects proof in the
 * background, with requests waiting. */
static gate *classifier_with_everything(uint64_t seed, uint32_t k) {
    gate *g = new_gate(GATE_KIND_CLASSIFIER, k);
    sim s = {seed, g->now_ms, 0};

    teach_until(&s, g, has_background);
    leave_waiting(&s, g);
    return g;
}

TEST(score_fresh) {
    gate *g = new_gate(GATE_KIND_SCORE, 0);

    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    CHECK(g->candidate && g->candidate->gen == 1 && g->candidate->samples == 0);
    CHECK_EQ_INT(g->npending, 0);
    check_round_trip(g, 1);
}

TEST(score_calibrating) {
    gate *g = score_with_requests(2, 0);
    waiting w = count_waiting(g);

    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    CHECK_EQ_INT(g->candidate->samples, 25);
    CHECK(g->candidate->regions[0].n == 25 && g->candidate->regions[0].errors > 0);
    CHECK(w.unanswered == 1 && w.answered == 2 && w.training == 0 && w.clues == 0);
    CHECK_EQ_INT(w.provisional, 0);
    check_round_trip(g, 2);
}

TEST(score_active) {
    gate *g = score_with_requests(3, 1);
    waiting w = count_waiting(g);

    CHECK(gateState(g) == GATE_STATE_ACTIVE);
    CHECK(g->candidate == NULL && g->serving->policy[0] >= 0);
    CHECK(g->serving->drift[0].n == g->serving->audits && g->serving->audits >= 5);
    CHECK(g->stats.accepted > 0 && g->stats.audited > 0 && g->stats.promotions == 1);
    CHECK(gateCoverage(g) > 0.0 && gateCoverage(g) < 1.0);
    CHECK(w.unanswered == 1 && w.answered == 2 && w.provisional == 0);
    check_round_trip(g, 3);
}

TEST(score_paused) {
    gate *g = new_gate(GATE_KIND_SCORE, 0);
    sim s = {4, g->now_ms, 0};
    uint64_t accepted;

    teach_until(&s, g, is_audited);
    accepted = g->stats.accepted;
    gatePause(g, s.now);
    teach(&s, g, 40);
    leave_waiting(&s, g);
    CHECK(gateState(g) == GATE_STATE_PAUSED);
    CHECK(g->serving != NULL);
    CHECK_EQ_INT(g->stats.accepted, accepted);
    CHECK(count_waiting(g).answered == 2);
    check_round_trip(g, 4);
}

TEST(score_after_drift) {
    gate *g = new_gate(GATE_KIND_SCORE, 0);
    sim s = {5, g->now_ms, 0};

    teach_until(&s, g, is_audited);
    s.wrong = 1;
    teach_until(&s, g, had_alarm);
    s.wrong = 0;
    teach(&s, g, 10);
    leave_waiting(&s, g);
    /* A score gate starts its next round of testing at once. */
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    CHECK_EQ_INT(g->stats.revocations, 1);
    CHECK_EQ_INT(g->generations, 2);
    CHECK(g->candidate && g->candidate->gen == 2 && g->candidate->samples >= 10);
    CHECK(!g->blind);
    check_round_trip(g, 5);
}

TEST(score_after_reset) {
    gate *g = new_gate(GATE_KIND_SCORE, 0);
    sim s = {6, g->now_ms, 0};

    teach_until(&s, g, is_audited);
    leave_waiting(&s, g);
    gateReset(g, s.now, 0x7265736574ull);
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    CHECK(g->npending == 0 && g->stats.requests == 0 && g->stats.promotions == 0);
    CHECK(g->generations == 2 && g->next_seq > 1 && g->epoch == 0x7265736574ull);
    check_round_trip(g, 6);
}

TEST(score_after_recover) {
    gate *g = new_gate(GATE_KIND_SCORE, 0);
    sim s = {7, g->now_ms, 0};

    teach_until(&s, g, is_audited);
    leave_waiting(&s, g);
    gateRecover(g, s.now, 0x7265636f766572ull);
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    CHECK(g->npending == 0 && g->stats.requests > 0 && g->stats.promotions == 1);
    CHECK(g->generations == 2 && g->candidate->gen == 2);
    check_round_trip(g, 7);
}

/* A reset or a recovery sets the clock instead of advancing it: a replica
 * that used to be a primary comes back to the clock of its new primary,
 * which may be behind its own. The gate is then older than its clock says. */
TEST(score_recovered_into_the_past) {
    gate *g = new_gate(GATE_KIND_SCORE, 0);
    sim s = {9, g->now_ms, 0};
    int64_t before = g->created_ms - 3600000;

    teach_until(&s, g, is_audited);
    leave_waiting(&s, g);
    gateRecover(g, before, 0x7265636f766572ull);
    CHECK_EQ_INT(g->now_ms, before);
    CHECK(g->created_ms > g->now_ms);
    CHECK(g->npending == 0 && g->candidate != NULL && g->candidate->frozen_ms == before);
    CHECK(ask_score(g, before + 1, 0.5, 0.9) != 0);
    CHECK_EQ_INT(g->newest->created_ms, before + 1);
    check_round_trip(g, 9);
}

TEST(classifier_fresh) {
    gate *g = new_gate(GATE_KIND_CLASSIFIER, 2);

    CHECK(gateState(g) == GATE_STATE_COLD);
    CHECK(g->trainer.w != NULL && g->trainer.updates == 0 && g->generations == 0);
    check_round_trip(g, 11);
}

TEST(classifier_learning) {
    gate *g = classifier_learning(12);
    waiting w = count_waiting(g);

    CHECK(gateState(g) == GATE_STATE_COLD);
    CHECK_EQ_INT(g->trainer.updates, 8);
    CHECK(w.training == 5 && w.clues == 5 && w.unanswered == 0 && w.answered == 0);
    CHECK_EQ_INT(g->nevidence, 0);
    CHECK(g->pending_bytes > 5 * sizeof(gateObs));
    check_round_trip(g, 12);
}

TEST(classifier_calibrating) {
    gate *g = new_gate(GATE_KIND_CLASSIFIER, 2);
    sim s = {13, g->now_ms, 0};
    waiting w;

    teach(&s, g, 19);
    for (int i = 0; i < 3; i++) sim_ask(&s, g); /* kept for training only */
    teach_until(&s, g, is_calibrating);
    teach(&s, g, 15);
    leave_waiting(&s, g);
    w = count_waiting(g);
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    CHECK(g->candidate->gen == 1 && g->candidate->samples >= 15 && g->candidate->budget > 0);
    CHECK(g->candidate->model.w != NULL && g->candidate->model.updates == 20);
    CHECK(w.training == 3 && w.unanswered == 1 && w.answered == 2 && w.clues == 4);
    check_round_trip(g, 13);
}

TEST(classifier_active) {
    gate *g = new_gate(GATE_KIND_CLASSIFIER, 2);
    sim s = {14, g->now_ms, 0};
    waiting w;

    teach_until(&s, g, is_audited);
    leave_waiting(&s, g);
    w = count_waiting(g);
    CHECK(gateState(g) == GATE_STATE_ACTIVE);
    CHECK(g->serving->policy[0] >= 0 || g->serving->policy[1] >= 0);
    CHECK(g->serving->audits >= 5 && g->stats.accepted > 0 && g->stats.promotions >= 1);
    CHECK(w.unanswered == 1 && w.answered == 2);
    check_round_trip(g, 14);
}

TEST(classifier_background_candidate) {
    gate *g = classifier_with_everything(15, 2);

    CHECK(gateState(g) == GATE_STATE_ACTIVE);
    CHECK(g->candidate->gen > g->serving->gen);
    CHECK(g->candidate->model.updates > g->serving->model.updates);
    CHECK(count_waiting(g).answered == 2);
    check_round_trip(g, 15);
}

/* While paused, a request the gate would have answered is recorded for
 * training instead. */
TEST(classifier_paused) {
    gate *g = new_gate(GATE_KIND_CLASSIFIER, 2);
    sim s = {16, g->now_ms, 0};
    uint64_t accepted;
    int would = 0;

    teach_until(&s, g, is_audited);
    accepted = g->stats.accepted;
    gatePause(g, s.now);
    teach(&s, g, 30);
    for (int i = 0; i < 200 && !would; i++) {
        asked a = sim_ask(&s, g);
        const gateObs *o = a.res.seq ? gateFindObservation(g, a.res.seq) : NULL;

        if (o && o->flags == GATE_OBS_WOULD_ACCEPT) would = 1;
        else if (o) gateLearn(g, s.now, g->epoch, o->seq, a.answer);
    }
    CHECK(would);
    CHECK(gateState(g) == GATE_STATE_PAUSED);
    CHECK_EQ_INT(g->stats.accepted, accepted);
    check_round_trip(g, 16);
}

TEST(classifier_after_drift) {
    gate *g = new_gate(GATE_KIND_CLASSIFIER, 2);
    sim s = {17, g->now_ms, 0};

    teach_until(&s, g, is_audited);
    s.wrong = 1;
    teach_until(&s, g, had_alarm);
    s.wrong = 0;
    teach(&s, g, 10);
    for (int i = 0; i < 3; i++) sim_ask(&s, g);
    /* The alarm wiped the model: the gate is learning again from scratch. */
    CHECK(gateState(g) == GATE_STATE_COLD);
    CHECK_EQ_INT(g->stats.revocations, 1);
    CHECK(g->trainer.updates > 0 && g->trainer.updates <= 10);
    CHECK(g->generations >= 1 && g->snapshot_updates == 0);
    CHECK(!g->blind);
    CHECK_EQ_INT(count_waiting(g).training, 3);
    check_round_trip(g, 17);
}

TEST(classifier_after_reset) {
    gate *g = new_gate(GATE_KIND_CLASSIFIER, 2);
    sim s = {18, g->now_ms, 0};
    uint64_t generations;

    teach_until(&s, g, is_audited);
    leave_waiting(&s, g);
    generations = g->generations;
    gateReset(g, s.now, 0x7265736574ull);
    CHECK(gateState(g) == GATE_STATE_COLD);
    CHECK(g->trainer.updates == 0 && g->npending == 0 && g->stats.learned == 0);
    CHECK(g->generations == generations && g->next_seq > 1);
    check_round_trip(g, 18);
}

TEST(classifier_after_recover) {
    gate *g = new_gate(GATE_KIND_CLASSIFIER, 2);
    sim s = {19, g->now_ms, 0};
    uint64_t generations, learned;

    teach_until(&s, g, is_audited);
    leave_waiting(&s, g);
    generations = g->generations;
    learned = g->trainer.updates;
    gateRecover(g, s.now, 0x7265636f766572ull);
    /* The model is kept and a fresh snapshot of it is being tested. */
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    CHECK(g->npending == 0 && g->trainer.updates == learned && g->stats.learned > 0);
    CHECK(g->generations == generations + 1 && g->candidate->gen == generations + 1);
    CHECK_EQ_INT(g->candidate->model.updates, learned);
    check_round_trip(g, 19);
}

/* Three classes: one weight vector per class instead of one in all, a
 * stricter label and a deferred one. */
TEST(classifier_three_classes) {
    gate *g = classifier_with_everything(20, 3);

    CHECK(gateState(g) == GATE_STATE_ACTIVE);
    CHECK_EQ_INT(g->trainer.nvec, 3);
    CHECK_EQ_INT(g->serving->policy[2], -1);
    CHECK_EQ_INT(g->serving->regions[2 * GATE_THRESHOLDS].n, 0);
    CHECK(count_waiting(g).answered == 2);
    check_round_trip(g, 20);
}

/* ------------------------- Late and lost answers -------------------------
 * The states of gate_core.h's EVIDENCE, MISSING and BLIND paragraphs, with
 * requests asked at chosen times. A score gate created by new_gate() tests
 * generation 1 and nothing answers, so every request is proof for it; its
 * feedback timeout is SIM_TIMEOUT and proof waits SIM_GRACE for an answer. */

/* A request at 10 ms that gets no answer, and three at 20, 21 and 22 ms
 * that are answered at once (the cheap result was right). All score 0.9
 * and none draws an audit. Returns the id of the first. */
static uint64_t ask_one_late(gate *g) {
    int64_t t0 = g->created_ms;
    uint64_t late = ask_score(g, t0 + 10, 0.9, 0.9);

    for (int i = 0; i < 3; i++) {
        uint64_t id = ask_score(g, t0 + 20 + i, 0.9, 0.9);

        MUST(id != 0 && gateLearn(g, t0 + 20 + i, g->epoch, id, 1) == 1);
    }
    return late;
}

/* Answers wait for the answer of an older request. */
TEST(score_answers_waiting) {
    gate *g = new_gate(GATE_KIND_SCORE, 0);
    int64_t t0 = g->created_ms;
    uint64_t late = ask_one_late(g);
    waiting w;

    /* One millisecond before proof stops waiting for the first request. */
    gateMaintain(g, t0 + 10 + SIM_GRACE - 1);
    w = count_waiting(g);
    CHECK(w.unanswered == 1 && w.answered == 3 && w.provisional == 0 && w.training == 0);
    CHECK_EQ_INT(g->npending, 4);
    CHECK_EQ_INT(g->nanswered, 3);
    CHECK_EQ_INT(g->nevidence, 4);
    CHECK_EQ_INT(gateUnanswered(g), 1);
    CHECK(gateFindObservation(g, late) == g->ehead);
    CHECK(g->oldest->flags == GATE_OBS_CAND);
    CHECK(g->newest->flags == (GATE_OBS_CAND | GATE_OBS_RESOLVED) && g->newest->result == 1);
    /* Nothing was counted out of turn. */
    CHECK_EQ_INT(g->candidate->samples, 0);
    CHECK_EQ_INT(gateMaintenanceDue(g, t0 + 10 + SIM_GRACE - 1), 0);
    CHECK_EQ_INT(gateMaintenanceDue(g, t0 + 10 + SIM_GRACE), 1);
    check_round_trip(g, 21);
}

/* A quarter of the feedback timeout later proof moves on without the late
 * answer: the request is counted as a wrong guess and keeps waiting. */
TEST(score_provisional) {
    static const double fraction[GATE_NBETS] = {0.1, 0.25, 0.5, 0.9};
    gate *g = new_gate(GATE_KIND_SCORE, 0), *copy;
    int64_t t0 = g->created_ms, now = t0 + 10 + SIM_GRACE;
    uint64_t late = ask_one_late(g), id;
    const gateObs *o;
    waiting w;

    gateMaintain(g, now);
    o = gateFindObservation(g, late);
    w = count_waiting(g);
    CHECK(o != NULL && o == g->oldest && o->flags == (GATE_OBS_CAND | GATE_OBS_PROVISIONAL));
    CHECK(o && o->result == -1 && o->cand_gen == 1);
    CHECK(w.provisional == 1 && w.gone == 0 && w.unanswered == 0 && w.answered == 0);
    CHECK(g->ehead == NULL && g->nevidence == 0 && g->nanswered == 0);
    CHECK_EQ_INT(g->npending, 1);
    CHECK_EQ_INT(gateUnanswered(g), 1);
    /* One wrong guess for now and three right ones. A score of 0.9 lies in
     * the regions whose threshold 1 - 0.002^(t/31) is at most 0.9: t up to
     * 11 (0.8898), not 12 (0.9098). */
    CHECK_EQ_INT(g->candidate->samples, 4);
    CHECK_EQ_INT(g->candidate->errors, 1);
    CHECK(g->candidate->regions[11].n == 4 && g->candidate->regions[11].errors == 1);
    CHECK_EQ_INT(g->candidate->regions[12].n, 0);
    CHECK(check_copy(g));

    /* What a copy does when the answer arrives after all and the cheap
     * result was right: the wealth of each region is what four right
     * guesses give, log(1 + c * alpha / (1 - alpha)) each, alpha 0.2. */
    copy = clone(g);
    CHECK(copy != NULL);
    if (copy != NULL) {
        CHECK_EQ_INT(gateLearn(copy, now + 1, copy->epoch, late, 1), 1);
        CHECK_EQ_INT(copy->npending, 0);
        CHECK_EQ_INT(copy->candidate->samples, 4);
        CHECK_EQ_INT(copy->candidate->errors, 0);
        CHECK_EQ_INT(copy->candidate->regions[0].errors, 0);
        for (int j = 0; j < GATE_NBETS; j++)
            CHECK_NEAR(copy->candidate->regions[11].logw[j],
                       4 * log1p(fraction[j] * 0.2 / 0.8), 1e-12);
        CHECK_EQ_INT(copy->stats.missing, 0);
        CHECK(check_copy(copy));
        gateFree(copy);
    }
    /* And when it was wrong: the request is released, nothing else changes. */
    copy = clone(g);
    CHECK(copy != NULL);
    if (copy != NULL) {
        CHECK_EQ_INT(gateLearn(copy, now + 1, copy->epoch, late, 0), 1);
        CHECK_EQ_INT(copy->npending, 0);
        CHECK_EQ_INT(copy->candidate->errors, 1);
        for (int j = 0; j < GATE_NBETS; j++)
            CHECK_NEAR(copy->candidate->regions[11].logw[j],
                       3 * log1p(fraction[j] * 0.2 / 0.8) + log1p(-fraction[j]), 1e-12);
        gateFree(copy);
    }
    /* And when it never arrives: at the feedback timeout the id is
     * forgotten and the request stays a wrong guess. */
    copy = clone(g);
    CHECK(copy != NULL);
    if (copy != NULL) {
        gateMaintain(copy, t0 + 10 + SIM_TIMEOUT - 1);
        CHECK_EQ_INT(copy->npending, 1);
        gateMaintain(copy, t0 + 10 + SIM_TIMEOUT);
        CHECK_EQ_INT(copy->npending, 0);
        CHECK_EQ_INT(copy->stats.missing, 1);
        CHECK_EQ_INT(copy->missing_run, 1);
        CHECK_EQ_INT(copy->candidate->samples, 4);
        CHECK_EQ_INT(copy->candidate->errors, 1);
        CHECK_EQ_INT(gateLearn(copy, t0 + 10 + SIM_TIMEOUT, copy->epoch, late, 1), 0);
        CHECK(check_copy(copy));
        gateFree(copy);
    }

    /* More proof queues up behind a newer request without an answer, with
     * the first one still waiting. */
    CHECK(ask_score(g, now + 1, 0.5, 0.9) != 0);
    id = ask_score(g, now + 2, 0.5, 0.9);
    CHECK_EQ_INT(gateLearn(g, now + 2, g->epoch, id, 1), 1);
    w = count_waiting(g);
    CHECK(w.provisional == 1 && w.unanswered == 1 && w.answered == 1);
    CHECK(g->ehead == g->oldest->next && g->nevidence == 2 && g->nanswered == 1);
    CHECK_EQ_INT(g->candidate->samples, 4);
    check_round_trip(g, 22);
}

/* A request the proof has moved on without outlives the generation it was
 * proof for: here a drift alarm drops it. The request waits on, for
 * nothing; when its time is up it is dropped, not missed. */
TEST(score_provisional_for_nothing) {
    gate *g = new_gate(GATE_KIND_SCORE, 0), *copy;
    sim s = {8, g->now_ms, 0};
    uint64_t late, missing, dropped;
    const gateObs *o;
    waiting w;

    teach_until(&s, g, is_audited);
    late = leave_provisional(&s, g);
    o = gateFindObservation(g, late);
    CHECK(o != NULL && (o->flags & GATE_OBS_PROVISIONAL) && is_live(g, o));
    s.wrong = 1;
    teach_until(&s, g, had_alarm);
    s.wrong = 0;
    o = gateFindObservation(g, late);
    w = count_waiting(g);
    CHECK(o != NULL && (o->flags & GATE_OBS_PROVISIONAL) && !is_live(g, o));
    CHECK(w.provisional == 1 && w.gone == 1);
    CHECK_EQ_INT(g->npending, 1);
    CHECK(g->candidate != NULL && g->serving == NULL && o && o->serv_gen < g->candidate->gen);
    CHECK(check_copy(g));

    copy = clone(g);
    CHECK(copy != NULL);
    if (copy != NULL && o != NULL) {
        missing = copy->stats.missing;
        dropped = copy->stats.dropped;
        gateMaintain(copy, o->created_ms + SIM_TIMEOUT);
        CHECK(gateFindObservation(copy, late) == NULL);
        CHECK_EQ_INT(copy->stats.missing, missing);
        CHECK_EQ_INT(copy->stats.dropped, dropped + 1);
        CHECK_EQ_INT(copy->missing_run, 0);
    }
    gateFree(copy);
    check_round_trip(g, 8);
}

/* The same in a classifier that answers while a newer snapshot is tested:
 * the request keeps its clues for the day its label arrives. */
TEST(classifier_provisional) {
    gate *g = classifier_with_everything(27, 2);
    sim s = {1027, g->now_ms, 0};
    uint64_t late = leave_provisional(&s, g), learned;
    const gateObs *o = gateFindObservation(g, late);
    waiting w = count_waiting(g);
    gate *copy;

    CHECK(o != NULL && (o->flags & GATE_OBS_PROVISIONAL) && !(o->flags & GATE_OBS_RESOLVED));
    CHECK(o && o->x.n > 0 && o->result == -1);
    CHECK(w.provisional >= 1 && w.answered == 0);
    CHECK(g->ehead != o);
    CHECK(check_copy(g));

    /* A copy learns from the label when it arrives, like the original. */
    copy = clone(g);
    CHECK(copy != NULL);
    if (copy != NULL) {
        learned = copy->trainer.updates;
        CHECK_EQ_INT(gateLearn(copy, s.now, copy->epoch, late, 0), 1);
        CHECK_EQ_INT(gateLearn(g, s.now, g->epoch, late, 0), 1);
        CHECK_EQ_INT(copy->trainer.updates, learned + 1);
        CHECK(gateFindObservation(copy, late) == NULL);
        CHECK_EQ_STR(gate_diff(g, copy), "");
        gateFree(copy);
    }
    check_round_trip(g, 27);
}

/* GATE_MISSING_RUN requests in a row end without an answer: the gate is
 * blind. It drops what it was testing, takes no snapshot, and records
 * every request so that there is an id to answer. */
static gate *score_blind(uint64_t *audited, uint64_t *plain) {
    gate *g = new_gate(GATE_KIND_SCORE, 0);
    int64_t t0 = g->created_ms;

    for (uint32_t i = 0; i < GATE_MISSING_RUN - 1; i++)
        MUST(ask_score(g, t0 + 1 + i, 0.9, 0.9) != 0);
    gateMaintain(g, t0 + 100 + SIM_TIMEOUT);
    /* One short of the run: the gate still tests, and remembers the run. */
    CHECK(!g->blind && g->candidate != NULL);
    CHECK_EQ_INT(g->missing_run, GATE_MISSING_RUN - 1);
    CHECK_EQ_INT(g->stats.missing, GATE_MISSING_RUN - 1);
    CHECK_EQ_INT(g->npending, 0);
    CHECK(g->candidate && g->candidate->samples == GATE_MISSING_RUN - 1);
    CHECK(g->candidate && g->candidate->errors == GATE_MISSING_RUN - 1);
    CHECK(check_copy(g));

    MUST(ask_score(g, t0 + 5000, 0.9, 0.9) != 0);
    gateMaintain(g, t0 + 5000 + SIM_TIMEOUT);
    CHECK_EQ_INT(g->blind, 1);
    CHECK_EQ_INT(g->missing_run, 0);
    CHECK_EQ_INT(g->stats.missing, GATE_MISSING_RUN);
    CHECK(g->candidate == NULL && g->serving == NULL);
    CHECK_EQ_INT(g->generations, 1);
    CHECK_EQ_INT(g->stats.revocations, 0); /* nothing was answering */
    CHECK_EQ_INT(g->npending, 0);
    CHECK(gateState(g) == GATE_STATE_COLD);
    CHECK_EQ_INT(gateMaintenanceDue(g, t0 + 9000), 0);
    CHECK(check_copy(g));

    *audited = ask_score(g, t0 + 9000, 0.4, 0.1);
    *plain = ask_score(g, t0 + 9001, 0.7, 0.9);
    return g;
}

TEST(score_blind) {
    uint64_t audited, plain;
    gate *g = score_blind(&audited, &plain);
    const gateObs *a = gateFindObservation(g, audited), *p = gateFindObservation(g, plain);
    waiting w = count_waiting(g);

    /* Recorded, but proof for nothing: a score gate has nothing to train
     * either, the ids are there to be answered. */
    CHECK(a != NULL && a->flags == GATE_OBS_AUDIT && a->cand_gen == 0 && a->serv_gen == 0);
    CHECK(p != NULL && p->flags == 0);
    CHECK(w.training == 2 && w.unanswered == 0 && w.provisional == 0);
    CHECK(g->blind && g->candidate == NULL && g->generations == 1);
    CHECK_EQ_INT(g->nevidence, 0);
    CHECK(gateState(g) == GATE_STATE_COLD);
    check_round_trip(g, 23);
}

/* An answer arrives: the gate sees again and the same call starts the
 * next round of testing. */
TEST(score_after_blind) {
    uint64_t audited, plain;
    gate *g = score_blind(&audited, &plain);
    const gateObs *a;

    CHECK_EQ_INT(gateLearn(g, g->now_ms + 5, g->epoch, plain, 1), 1);
    a = gateFindObservation(g, audited);
    CHECK(!g->blind && g->missing_run == 0);
    CHECK(g->candidate != NULL && g->candidate->gen == 2 && g->candidate->samples == 0);
    CHECK_EQ_INT(g->generations, 2);
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    /* The other request of the blind time still waits, proof for nothing. */
    CHECK_EQ_INT(g->npending, 1);
    CHECK(a != NULL && a->flags == GATE_OBS_AUDIT && a->cand_gen == 0);
    CHECK_EQ_INT(g->nevidence, 0);
    check_round_trip(g, 24);
}

/* What a gate was like before it lost its teacher. */
typedef struct before {
    uint64_t learned, generations, revocations;
} before;

/* A classifier that answered, then lost its teacher: the proof goes, the
 * model stays, and what is asked while blind is kept with its clues. */
static gate *classifier_blind(uint64_t seed, before *was) {
    gate *g = new_gate(GATE_KIND_CLASSIFIER, 2);
    sim s = {seed, g->now_ms, 0};

    teach_until(&s, g, is_audited);
    was->learned = g->trainer.updates;
    was->generations = g->generations;
    was->revocations = g->stats.revocations;
    go_silent(&s, g, 250);
    for (int i = 0; i < 4; i++) sim_ask(&s, g);
    return g;
}

TEST(classifier_blind) {
    before was;
    gate *g = classifier_blind(25, &was);
    uint64_t learned = was.learned, generations = was.generations;
    waiting w = count_waiting(g);
    gateSparse x;
    uint64_t rng = 25;

    CHECK_EQ_INT(g->blind, 1);
    CHECK(g->candidate == NULL && g->serving == NULL);
    CHECK_EQ_INT(g->stats.revocations, was.revocations + 1); /* it was answering */
    CHECK(g->stats.missing >= GATE_MISSING_RUN);
    CHECK(g->trainer.updates == learned && learned >= 20);
    CHECK_EQ_INT(g->generations, generations);
    CHECK_EQ_INT(g->snapshot_updates, 0);
    CHECK(w.training == 4 && w.clues == 4 && w.unanswered == 0 && w.answered == 0);
    CHECK_EQ_INT(g->nevidence, 0);
    CHECK(gateState(g) == GATE_STATE_COLD);
    CHECK(check_copy(g));

    /* Examples taught meanwhile (GADD) do not make it take a snapshot. */
    for (int i = 0; i < 25; i++) {
        make_clues(&rng, (uint32_t)i % 2, &x);
        CHECK(gateTrain(g, g->now_ms + 1, &x, i % 2) == GATE_OK);
        gateSparseFree(&x);
    }
    CHECK_EQ_INT(g->trainer.updates, learned + 25);
    CHECK(g->blind && g->candidate == NULL && g->generations == generations);
    check_round_trip(g, 25);
}

TEST(classifier_after_blind) {
    before was;
    gate *g = classifier_blind(26, &was);
    uint64_t learned = was.learned, generations = was.generations;
    uint64_t id = g->newest ? g->newest->seq : 0;

    CHECK(g->blind && id != 0);
    CHECK_EQ_INT(gateLearn(g, g->now_ms + 1, g->epoch, id, 1), 1);
    /* The label was learned from, and the snapshot that was due is of the
     * model that knows it. */
    CHECK(!g->blind && g->missing_run == 0);
    CHECK_EQ_INT(g->trainer.updates, learned + 1);
    CHECK(g->candidate != NULL && g->serving == NULL);
    CHECK(g->candidate && g->candidate->gen == generations + 1);
    CHECK(g->candidate && g->candidate->model.updates == learned + 1);
    CHECK_EQ_INT(g->snapshot_updates, learned + 1);
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    CHECK_EQ_INT(count_waiting(g).training, 3);
    check_round_trip(g, 26);
}

/* A blind gate may be paused as well, and a paused one go blind. */
TEST(blind_and_paused) {
    uint64_t audited, plain;
    gate *g = score_blind(&audited, &plain);

    gatePause(g, g->now_ms);
    CHECK(g->blind && g->paused);
    CHECK(gateState(g) == GATE_STATE_PAUSED);
    check_round_trip(g, 28);
}

/* --------------------------------- Limits -------------------------------- */

/* gateDecode() must refuse these bytes, for exactly this reason. */
#define REJECT(ptr, len, reason) do { \
    char _err[160] = ""; \
    gate *_g = gateDecode((ptr), (len), _err, sizeof(_err)); \
    CHECK(_g == NULL); \
    CHECK_EQ_STR(_err, (reason)); \
    gateFree(_g); \
} while (0)

/* The same when the reason depends on what the misread bytes happen to
 * look like: any reason will do. */
#define REJECT_ANY(ptr, len) do { \
    char _err[160] = ""; \
    gate *_g = gateDecode((ptr), (len), _err, sizeof(_err)); \
    CHECK(_g == NULL); \
    CHECK(_err[0] != '\0'); \
    gateFree(_g); \
} while (0)

/* The largest classifier there can be: 64 classes with 64-byte labels, the
 * trainer and a frozen copy of it. */
static gate *widest_gate(void) {
    char names[GATE_MAX_CLASSES][GATE_MAX_LABEL_BYTES + 1];
    const char *labels[GATE_MAX_CLASSES];
    size_t lens[GATE_MAX_CLASSES];
    gateSparse x = {0, NULL, NULL};
    uint64_t rng = 64;
    gateConfig cfg;
    char err[160] = "";
    gate *g;

    for (uint32_t i = 0; i < GATE_MAX_CLASSES; i++) {
        memset(names[i], 'a' + (int)(i % 26), GATE_MAX_LABEL_BYTES);
        names[i][GATE_MAX_LABEL_BYTES - 2] = (char)('0' + i / 10);
        names[i][GATE_MAX_LABEL_BYTES - 1] = (char)('0' + i % 10);
        names[i][GATE_MAX_LABEL_BYTES] = '\0';
        labels[i] = names[i];
        lens[i] = GATE_MAX_LABEL_BYTES;
    }
    gateConfigInit(&cfg);
    cfg.kind = GATE_KIND_CLASSIFIER;
    cfg.alpha = 0.2;
    cfg.delta = 0.2;
    cfg.audit = 0.3;
    cfg.snapshot_examples = 1;
    cfg.feedback_timeout_ms = 5000;
    MUST(gateConfigSetClasses(&cfg, labels, lens, GATE_MAX_CLASSES) == GATE_OK);
    MUST(gateConfigValidate(&cfg, err, sizeof(err)) == GATE_OK);
    g = gateCreate(&cfg, 64, 46, 1700000000000ll);
    gateConfigFree(&cfg);
    make_clues(&rng, 63, &x);
    MUST(gateTrain(g, g->now_ms + 1, &x, 63) == GATE_OK);
    gateSparseFree(&x);
    return g;
}

TEST(widest_classifier) {
    gate *g = widest_gate();

    CHECK(g->candidate != NULL && g->trainer.nvec == GATE_MAX_CLASSES);
    /* 8 + config (41 + 64 * (4 + 64) + 64 * 9) + 54 + trainer (1 + 2097416)
     * + candidate (1 + 72 + 2097416 + 64 * 1824) + 1 + 12 + 120 bytes,
     * a model being 8 + 64 * 8192 * 4 + 64 * 4 = 2097416. */
    CHECK_EQ_INT(layout_of(g).end, 4316806);
    check_round_trip(g, 64);
}

/* A copy of a full gate must be the same gate; with one more request,
 * linked behind the back of the core, it must not decode. Releases both. */
static void check_full(gate *g) {
    gate *copy = clone(g);
    blob b;

    CHECK(gateFull(g));
    CHECK(copy != NULL);
    if (copy != NULL) {
        CHECK_EQ_STR(gate_diff(g, copy), "");
        gateObsLink(copy, new_request(copy->next_seq++, copy->now_ms, 0));
        b = encode(copy, 0);
        REJECT(b.p, b.n, "too many waiting requests");
        blob_free(&b);
        gateFree(copy);
    }
    gateFree(g);
}

/* 'n' requests of a score gate that get no answer, all at the gate's own
 * time. The gate takes what it has room for. */
static void ask_unanswered(gate *g, uint32_t n) {
    gatePredictResult res;
    gateEval ev;

    for (uint32_t i = 0; i < n; i++) {
        MUST(gateEvaluate(g, NULL, 0.5, &ev) == GATE_OK);
        MUST(gatePredict(g, g->now_ms, 0.9, &ev, NULL, &res) == GATE_OK);
    }
}

/* A gate that holds as many requests as it may is still a gate. */
TEST(full_of_requests) {
    gate *g = new_gate(GATE_KIND_SCORE, 0);

    ask_unanswered(g, GATE_MAX_PENDING + 5);
    CHECK_EQ_INT(g->npending, GATE_MAX_PENDING);
    CHECK_EQ_INT(g->stats.full, 5);
    check_full(g);
}

/* The same for the limit on stored clues. The request that makes the gate
 * full is recorded whole, so a full gate holds more than
 * GATE_MAX_PENDING_BYTES: up to one request more. 256 requests of 8192
 * clues do it: 255 * (sizeof(gateObs) + 65536) is still below 16 MB. */
TEST(full_of_clues) {
    gate *g = new_gate(GATE_KIND_CLASSIFIER, 2);
    gatePredictResult res;
    gateEval ev;

    while (!gateFull(g) && g->npending < 1000) {
        gateSparse x;

        x.n = GATE_DIM;
        x.idx = gate_malloc(sizeof(uint32_t) * GATE_DIM);
        x.val = gate_malloc(sizeof(float) * GATE_DIM);
        for (uint32_t i = 0; i < GATE_DIM; i++) {
            x.idx[i] = i;
            x.val[i] = i % 2 ? 0.01f : -0.01f;
        }
        MUST(gateEvaluate(g, &x, 0, &ev) == GATE_OK);
        MUST(gatePredict(g, g->now_ms, 0.9, &ev, &x, &res) == GATE_OK && res.seq != 0);
        gateSparseFree(&x);
    }
    CHECK_EQ_INT(g->npending, 256);
    CHECK(g->pending_bytes > GATE_MAX_PENDING_BYTES);
    CHECK(g->pending_bytes - GATE_OBS_BYTES(GATE_DIM) < GATE_MAX_PENDING_BYTES);
    CHECK_EQ_INT(g->newest->x.idx[GATE_DIM - 1], GATE_DIM - 1);
    check_full(g);
}

/* As many answers waiting for an older one as the core lets wait: one
 * request without an answer and GATE_MAX_WAITING answered ones behind it,
 * all asked at the same time. The feedback timeout is a week, so proof
 * does not move on for age. */
static gate *gate_with_answers_waiting(void) {
    gate *g = new_gate_with(GATE_KIND_SCORE, 0, GATE_MAX_FEEDBACK_TIMEOUT_MS);
    int64_t now = g->now_ms;

    MUST(ask_score(g, now, 0.9, 0.9) != 0);
    for (uint32_t i = 0; i < GATE_MAX_WAITING; i++) {
        uint64_t id = ask_score(g, now, 0.25, 0.9);

        MUST(id != 0 && gateLearn(g, now, g->epoch, id, 0) == 1);
    }
    return g;
}

/* Answered requests do not make a gate full, but only so many wait: the
 * core holds exactly GATE_MAX_WAITING, the decoder takes exactly that. */
TEST(full_of_answers) {
    gate *g = gate_with_answers_waiting(), *copy = clone(g);
    uint64_t id;
    gateObs *o;
    blob b;

    CHECK_EQ_INT(g->nanswered, GATE_MAX_WAITING);
    CHECK_EQ_INT(g->npending, GATE_MAX_WAITING + 1);
    CHECK_EQ_INT(gateUnanswered(g), 1);
    CHECK(!gateFull(g));
    CHECK_EQ_INT(g->candidate->samples, 0);
    /* 2134 bytes without requests (8 + 41 + 54 + 1 + 1897 + 1 + 12 + 120). */
    CHECK_EQ_INT(layout_of(g).end, 2134 + (GATE_MAX_WAITING + 1) * 68);
    CHECK(check_copy(g));
    CHECK(copy != NULL);
    if (copy != NULL) {
        CHECK_EQ_STR(gate_diff(g, copy), "");
        /* One more answer, linked behind the back of the core. */
        o = new_request(copy->next_seq++, copy->now_ms, GATE_OBS_CAND | GATE_OBS_RESOLVED);
        o->cand_gen = 1;
        o->result = 1;
        gateObsLink(copy, o);
        b = encode(copy, 0);
        REJECT(b.p, b.n, "too many answers waiting");
        blob_free(&b);
        gateFree(copy);
    }

    /* Through the core, the answer that is one too many makes proof move
     * on without the first request: nothing waits for its turn any more. */
    id = ask_score(g, g->now_ms, 0.25, 0.9);
    CHECK_EQ_INT(gateLearn(g, g->now_ms, g->epoch, id, 0), 1);
    CHECK_EQ_INT(g->nanswered, 0);
    CHECK_EQ_INT(g->nevidence, 0);
    CHECK_EQ_INT(g->npending, 1);
    CHECK(g->oldest && g->oldest->flags == (GATE_OBS_CAND | GATE_OBS_PROVISIONAL));
    CHECK_EQ_INT(g->candidate->samples, GATE_MAX_WAITING + 2);
    CHECK_EQ_INT(g->candidate->errors, GATE_MAX_WAITING + 2);
    check_round_trip(g, 29);
}

/* The most a gate can hold: GATE_MAX_WAITING answers waiting behind a
 * request without one, and as many more unanswered requests as make the
 * gate full. Neither limit counts against the other. */
TEST(full_of_both) {
    gate *g = gate_with_answers_waiting();

    ask_unanswered(g, GATE_MAX_PENDING + 5);
    CHECK_EQ_INT(gateUnanswered(g), GATE_MAX_PENDING);
    CHECK_EQ_INT(g->nanswered, GATE_MAX_WAITING);
    CHECK_EQ_INT(g->npending, GATE_MAX_PENDING + GATE_MAX_WAITING);
    CHECK_EQ_INT(g->stats.full, 6);
    check_full(g);
}

/* A gate that was full, got answers for its newest requests and filled up
 * again holds answered requests between unanswered ones, more than
 * GATE_MAX_PENDING in all, and the request that fills it comes last. */
TEST(full_again) {
    gate *g = new_gate_with(GATE_KIND_SCORE, 0, GATE_MAX_FEEDBACK_TIMEOUT_MS);
    uint64_t newest;

    ask_unanswered(g, GATE_MAX_PENDING);
    CHECK(gateFull(g));
    newest = g->newest->seq;
    for (uint64_t id = newest; id > newest - 100; id--)
        CHECK_EQ_INT(gateLearn(g, g->now_ms, g->epoch, id, 1), 1);
    CHECK(!gateFull(g));
    CHECK_EQ_INT(g->nanswered, 100);
    ask_unanswered(g, 150);
    CHECK_EQ_INT(g->npending, GATE_MAX_PENDING + 100);
    CHECK_EQ_INT(gateUnanswered(g), GATE_MAX_PENDING);
    CHECK_EQ_INT(g->stats.full, 50);
    CHECK(g->newest->flags == GATE_OBS_CAND && g->newest->prev->flags == GATE_OBS_CAND);
    check_full(g);
}

/* ---------------------------- Use after decoding -------------------------
 * Whatever the decoder lets through is a gate, and must be safe to use. */

/* What gate_core.h promises about a blind gate: it records what it is
 * asked while it has room, so that there is an id to answer, and the
 * answer makes it see again; a score gate starts testing in the same call. */
static void see_again(sim *s, gate *g) {
    asked a = sim_ask_now(s, g);

    MUST(a.res.seq != 0 || gateFull(g) || gateUnanswered(g) >= GATE_TRAIN_PENDING);
    if (a.res.seq == 0) return;
    MUST(gateLearn(g, s->now, g->epoch, a.res.seq, a.answer) == 1);
    MUST(!g->blind && g->missing_run == 0);
    MUST(g->cfg.kind != GATE_KIND_SCORE || g->candidate != NULL || g->serving != NULL);
}

/* This is what happens to every hostile blob that decodes. If the gate is
 * blind, a request and its answer. Then the answers of its newer requests
 * arrive (newest first, so that they pile up behind the oldest), proof
 * gives up waiting for the others, their answers arrive late, some more
 * traffic, and then silence until everything has timed out; and if that
 * leaves the gate blind, a request and its answer again. The gate must be
 * consistent throughout and, if 'again' is set, encode to something that
 * decodes. */
static void exercise(gate *g, uint64_t seed, int again) {
    uint64_t answers = g->cfg.kind == GATE_KIND_CLASSIFIER ? g->cfg.nclasses : 2;
    int64_t grace = g->cfg.feedback_timeout_ms / GATE_GRACE_DIVISOR;
    /* A counter the decoder bounds may be put at the bound by a blob and
     * then counted past it by the core: the one edge the decoder cannot
     * close on its own (the core would have to stop counting there). Such
     * a gate works, but its encoding is refused. Nothing here counts
     * further than this from where a blob may start. */
    uint64_t ids[32], limit = ((uint64_t)1 << 62) - 4096;
    sim s = {seed, g->now_ms, 0};
    char err[160] = "";
    int n = 0;
    blob b;
    gate *copy;

    for (const gateObs *o = g->oldest; o && n < 32; o = o->next)
        if (!(o->flags & GATE_OBS_RESOLVED)) ids[n++] = o->seq;
    if (g->blind) see_again(&s, g);
    MUST(gateCheckInvariants(g, err, sizeof(err)));
    for (int i = n; i > n / 2; i--)
        gateLearn(g, s.now, g->epoch, ids[i - 1], (int32_t)(ids[i - 1] % answers));
    MUST(gateCheckInvariants(g, err, sizeof(err)));
    /* The clock of a blob may leave no room to move. */
    s.now = s.now > LAST_MS - grace ? LAST_MS : s.now + grace;
    gateMaintain(g, s.now);
    MUST(gateCheckInvariants(g, err, sizeof(err)));
    for (int i = n / 2; i > 0; i--)
        gateLearn(g, s.now, g->epoch, ids[i - 1], (int32_t)(ids[i - 1] % answers));
    MUST(gateCheckInvariants(g, err, sizeof(err)));
    if (s.now < LAST_MS - 1000000) {
        teach(&s, g, 4);
        sim_ask(&s, g);
        sim_ask(&s, g);
    }
    gateMaintain(g, LAST_MS);
    MUST(gateCheckInvariants(g, err, sizeof(err)));
    if (g->blind) {
        s.now = LAST_MS;
        see_again(&s, g);
        MUST(gateCheckInvariants(g, err, sizeof(err)));
    }
    if (!again) return;

    /* Every counter gateDecode() bounds. */
    if (g->next_seq >= limit || g->generations >= limit || g->trainer.updates >= limit) return;
    if (g->stats.requests >= limit || g->stats.accepted >= limit || g->stats.audited >= limit ||
        g->stats.abstained >= limit || g->stats.missing >= limit || g->stats.revocations >= limit)
        return;
    if (g->candidate && (g->candidate->samples >= limit || g->candidate->audits >= limit)) return;
    if (g->serving && (g->serving->samples >= limit || g->serving->audits >= limit)) return;
    b = encode(g, 0);
    copy = gateDecode(b.p, b.n, err, sizeof(err));
    MUST(copy != NULL);
    gateFree(copy);
    blob_free(&b);
}

/* ------------------------------ Hostile blobs ---------------------------- */

/* A copy of 'base' is changed in memory (the statements see it as 'g') and
 * encoded: the encoder writes what it is given. The result must not decode. */
#define HOSTILE(base, reason, ...) do { \
    gate *g = clone(base); \
    CHECK(g != NULL); \
    if (g != NULL) { \
        blob _blob; \
        __VA_ARGS__; \
        _blob = encode(g, 0); \
        REJECT(_blob.p, _blob.n, reason); \
        blob_free(&_blob); \
        gateFree(g); \
    } \
} while (0)

/* The same for a change that is unusual but legitimate: it must decode,
 * encode back to the same bytes, and be a gate that can be used. */
#define ACCEPTED(base, ...) do { \
    gate *g = clone(base); \
    CHECK(g != NULL); \
    if (g != NULL) { \
        blob _blob, _blob2; \
        gate *_copy; \
        __VA_ARGS__; \
        _blob = encode(g, 0); \
        _copy = decode(&_blob); \
        CHECK(_copy != NULL); \
        if (_copy != NULL) { \
            _blob2 = encode(_copy, 0); \
            CHECK(blob_equal(&_blob, &_blob2)); \
            blob_free(&_blob2); \
            exercise(_copy, 77, 0); \
            gateFree(_copy); \
        } \
        blob_free(&_blob); \
        gateFree(g); \
    } \
} while (0)

/* The encoding of 'base' with 'width' bytes at 'at' overwritten. */
#define PATCHED(base, at, value, width, reason) do { \
    blob _blob = encode(base, 0); \
    put(_blob.p + (at), (value), (width)); \
    REJECT(_blob.p, _blob.n, reason); \
    blob_free(&_blob); \
} while (0)

static void set_label(gate *g, uint32_t i, const char *label) {
    gate_free(g->cfg.labels[i]);
    g->cfg.labels[i] = gate_malloc(strlen(label) + 1);
    strcpy(g->cfg.labels[i], label);
}

static void set_all_times(gate *g, int64_t ms) {
    g->created_ms = g->now_ms = ms;
    g->candidate->frozen_ms = g->serving->frozen_ms = g->serving->promoted_ms = ms;
    for (gateObs *o = g->oldest; o; o = o->next) o->created_ms = ms;
}

/* The decoded form of a gate changed in memory, NULL if it does not decode. */
#define DECODED(into, base, ...) do { \
    gate *g = clone(base); \
    (into) = NULL; \
    CHECK(g != NULL); \
    if (g != NULL) { \
        blob _blob; \
        __VA_ARGS__; \
        _blob = encode(g, 0); \
        (into) = decode(&_blob); \
        CHECK((into) != NULL); \
        blob_free(&_blob); \
        gateFree(g); \
    } \
} while (0)

TEST(hostile_header) {
    gate *ref = golden_score_gate();
    blob b = encode(ref, 0);
    unsigned char *longer = malloc(b.n + 1);

    PATCHED(ref, 0, 'g', 1, "bad magic");
    PATCHED(ref, 3, 0, 1, "bad magic");
    PATCHED(ref, 4, 2, 2, "unsupported codec version");
    PATCHED(ref, 4, 0, 2, "unsupported codec version");
    PATCHED(ref, 4, 0x0100, 2, "unsupported codec version"); /* 1, big endian */
    PATCHED(ref, 4, 0x0101, 2, "unsupported codec version");
    PATCHED(ref, 6, 2, 2, "unsupported features version");
    PATCHED(ref, 6, 0, 2, "unsupported features version");
    PATCHED(ref, 6, 0x0101, 2, "unsupported features version");

    REJECT(NULL, 0, "truncated");
    REJECT(b.p, 0, "truncated");
    REJECT(b.p, 3, "truncated");
    REJECT(b.p, b.n - 1, "truncated");
    memcpy(longer, b.p, b.n);
    longer[b.n] = 0;
    REJECT(longer, b.n + 1, "trailing bytes");
    free(longer);
    blob_free(&b);
    gateFree(ref);
}

/* The layout of a blob says what version 1 looks like now. Before the gate
 * knew about lost answers the ledger had no blind flag and no run of
 * missing answers, under the same version number. A blob of that layout,
 * here the encodings of today without those five bytes, must not get in as
 * something else. */
TEST(previous_layout) {
    gate *gates[] = {
        new_gate(GATE_KIND_SCORE, 0), score_with_requests(35, 1),
        new_gate(GATE_KIND_CLASSIFIER, 2), classifier_learning(36),
        classifier_with_everything(37, 2), golden_score_gate(), golden_classifier_gate(1),
        golden_blind_gate(),
    };

    for (size_t i = 0; i < COUNTOF(gates); i++) {
        blob b = encode(gates[i], 0);
        size_t at = layout_of(gates[i]).ledger + LEDGER_BLIND;

        memmove(b.p + at, b.p + at + 5, b.n - at - 5);
        REJECT_ANY(b.p, b.n - 5);
        blob_free(&b);
        gateFree(gates[i]);
    }
}

/* The pause flag and the blind flag are truth values to the core, whatever
 * the int holds: equal states give equal bytes, a 0 or a 1. */
TEST(truth_values) {
    gate *g = golden_blind_gate(), *copy;
    layout l = layout_of(g);
    blob one, other;

    g->paused = 1;
    one = encode(g, 0);
    g->paused = 2;
    g->blind = -1;
    other = encode(g, 0);
    CHECK(blob_equal(&one, &other));
    CHECK_EQ_INT(other.p[l.ledger + LEDGER_PAUSED], 1);
    CHECK_EQ_INT(other.p[l.ledger + LEDGER_BLIND], 1);
    copy = decode(&other);
    CHECK(copy != NULL && copy->paused == 1 && copy->blind == 1);
    gateFree(copy);
    blob_free(&one);
    blob_free(&other);
    gateFree(g);
}

TEST(hostile_config) {
    gate *ref = golden_classifier_gate(1), *few;
    char label65[66];
    blob b;

    memset(label65, 'x', 65);
    label65[65] = '\0';

    HOSTILE(ref, "invalid gate kind", g->cfg.kind = 2);
    HOSTILE(ref, "ALPHA must be greater than 0 and less than 1", g->cfg.alpha = 0.0);
    HOSTILE(ref, "ALPHA must be greater than 0 and less than 1", g->cfg.alpha = 1.0);
    HOSTILE(ref, "non-finite number", g->cfg.alpha = NAN);
    HOSTILE(ref, "DELTA must be greater than 0 and less than 1", g->cfg.delta = -0.5);
    HOSTILE(ref, "DELTA must be greater than 0 and less than 1", g->cfg.delta = 1.0);
    HOSTILE(ref, "non-finite number", g->cfg.delta = INFINITY);
    HOSTILE(ref, "AUDIT must be greater than 0 and at most 1", g->cfg.audit = 0.0);
    HOSTILE(ref, "AUDIT must be greater than 0 and at most 1", g->cfg.audit = 1.0000001);
    HOSTILE(ref, "non-finite number", g->cfg.audit = -INFINITY);
    HOSTILE(ref, "the snapshot step must be at least 1 example", g->cfg.snapshot_examples = 0);
    HOSTILE(ref, "the feedback timeout is out of range", g->cfg.feedback_timeout_ms = 0);
    HOSTILE(ref, "the feedback timeout is out of range", g->cfg.feedback_timeout_ms = -300000);
    HOSTILE(ref, "the feedback timeout is out of range",
            g->cfg.feedback_timeout_ms = GATE_MAX_FEEDBACK_TIMEOUT_MS + 1);
    /* All 64 bits of it: the low half of this one is the 300000 it was. */
    HOSTILE(ref, "the feedback timeout is out of range",
            g->cfg.feedback_timeout_ms += (int64_t)1 << 32);

    /* A class alpha of 0 would be replaced by ALPHA on the way in. */
    HOSTILE(ref, "invalid class alpha", g->cfg.class_alpha[1] = 0.0);
    HOSTILE(ref, "invalid class alpha", g->cfg.class_alpha[2] = -0.0);
    HOSTILE(ref, "non-finite number", g->cfg.class_alpha[0] = NAN);
    HOSTILE(ref, "CLASSALPHA for 'ham' must be greater than 0 and at most ALPHA",
            g->cfg.class_alpha[1] = 0.11);
    HOSTILE(ref, "CLASSALPHA for 'spam' must be greater than 0 and at most ALPHA",
            g->cfg.class_alpha[0] = -0.05);
    HOSTILE(ref, "invalid flag", g->cfg.class_defer[0] = 2);
    HOSTILE(ref, "ALWAYSDEFER cannot cover every label",
            g->cfg.class_defer[0] = g->cfg.class_defer[1] = 1);

    HOSTILE(ref, "invalid label", g->cfg.labels[0][1] = '\n');
    HOSTILE(ref, "invalid label", g->cfg.labels[0][0] = '\xff');
    HOSTILE(ref, "invalid label", g->cfg.labels[2][1] = 'e'); /* cuts the e-acute in two */
    HOSTILE(ref, "invalid label", g->cfg.labels[1][0] = '\0');
    HOSTILE(ref, "invalid label", set_label(g, 1, label65));
    HOSTILE(ref, "duplicate label 'spam'", set_label(g, 1, "spam"));
    /* The label "spam" starts at byte 41: a zero byte inside it. */
    PATCHED(ref, 42, 0, 1, "invalid label");
    /* Its length, at byte 37. */
    PATCHED(ref, 37, 0, 4, "invalid label");
    PATCHED(ref, 37, 65, 4, "invalid label");
    PATCHED(ref, 37, 0xffffffffu, 4, "truncated");
    /* The number of classes, at byte 33. */
    PATCHED(ref, 33, 65, 4, "too many classes");
    PATCHED(ref, 33, 0xffffffffu, 4, "too many classes");
    /* The kind, at byte 8: a score gate with labels. */
    PATCHED(ref, 8, GATE_KIND_SCORE, 1, "a SCORE gate has no classes");

    /* Fewer classes than a classifier can have. */
    for (uint32_t k = 0; k < 2; k++) {
        few = clone(ref);
        few->cfg.nclasses = k;
        b = encode(few, 0);
        few->cfg.nclasses = 3;
        REJECT(b.p, b.n, "CLASSES must list between 2 and 64 labels");
        blob_free(&b);
        gateFree(few);
    }
    gateFree(ref);
}

TEST(hostile_ledger_and_models) {
    gate *ref = golden_classifier_gate(1), *bare = golden_classifier_gate(0);
    gate *score = golden_score_gate(), *fresh = new_gate(GATE_KIND_SCORE, 0);
    layout l = layout_of(ref), ls = layout_of(score), lf = layout_of(fresh);
    const uint64_t top = (uint64_t)1 << 62;

    PATCHED(ref, l.ledger + LEDGER_PAUSED, 2, 1, "invalid flag");
    /* The clock stays in its range (TIME in gate_core.h), at both ends. */
    HOSTILE(bare, "clock out of range", g->now_ms = INT64_MAX);
    HOSTILE(bare, "clock out of range", g->now_ms = LAST_MS + 1);
    HOSTILE(bare, "clock out of range", g->now_ms = -1);
    HOSTILE(bare, "clock out of range", g->now_ms = INT64_MIN);
    /* Counters that others are compared with stop short of wrapping. */
    HOSTILE(ref, "counter out of range", g->generations = top + 1);
    HOSTILE(ref, "counter out of range", g->generations = (uint64_t)1 << 63);
    HOSTILE(ref, "counter out of range", g->next_seq = top + 1);
    HOSTILE(ref, "counter out of range", g->next_seq = ~0ull);
    HOSTILE(ref, "invalid request counter", g->next_seq = 0);

    /* BLIND: a flag; a blind gate holds no generation, whichever it is. */
    PATCHED(ref, l.ledger + LEDGER_BLIND, 2, 1, "invalid flag");
    PATCHED(ref, l.ledger + LEDGER_BLIND, 0x80, 1, "invalid flag");
    PATCHED(ref, l.ledger + LEDGER_BLIND, 1, 1, "a blind gate holds a generation");
    PATCHED(score, ls.ledger + LEDGER_BLIND, 1, 1, "a blind gate holds a generation");
    PATCHED(fresh, lf.ledger + LEDGER_BLIND, 1, 1, "a blind gate holds a generation");
    CHECK(fresh->candidate != NULL && fresh->serving == NULL && score->candidate == NULL);
    /* The run of missing answers ends in an alarm when it reaches
     * GATE_MISSING_RUN: it never rests there. Four bytes, little endian. */
    PATCHED(ref, l.ledger + LEDGER_MISSING_RUN, GATE_MISSING_RUN, 4, "counter out of range");
    PATCHED(ref, l.ledger + LEDGER_MISSING_RUN, GATE_MISSING_RUN + 1, 4, "counter out of range");
    PATCHED(ref, l.ledger + LEDGER_MISSING_RUN, 0xffffffffu, 4, "counter out of range");
    PATCHED(ref, l.ledger + LEDGER_MISSING_RUN, 0x100, 4, "counter out of range");
    PATCHED(ref, l.ledger + LEDGER_MISSING_RUN, 0x01000000, 4, "counter out of range");
    HOSTILE(score, "counter out of range", g->missing_run = GATE_MISSING_RUN);

    PATCHED(ref, l.trainer, 0, 1, "trainer does not match the gate kind");
    PATCHED(ref, l.trainer, 2, 1, "invalid flag");
    PATCHED(score, ls.trainer, 1, 1, "trainer does not match the gate kind");
    /* The number of examples a model has learned from is shown to clients
     * (GINFO, GADD), as a signed number. */
    HOSTILE(ref, "counter out of range", g->trainer.updates = top + 1);
    HOSTILE(ref, "counter out of range", g->trainer.updates = (uint64_t)1 << 63);
    HOSTILE(ref, "counter out of range", g->trainer.updates = ~0ull);
    HOSTILE(ref, "counter out of range", g->candidate->model.updates = top + 1);
    HOSTILE(ref, "counter out of range", g->serving->model.updates = ~0ull);
    HOSTILE(ref, "weight out of range", g->trainer.w[100] = NAN);
    HOSTILE(ref, "weight out of range", g->trainer.w[0] = INFINITY);
    HOSTILE(ref, "weight out of range", g->trainer.w[24575] = 64.00001f);
    HOSTILE(ref, "weight out of range", g->trainer.b[1] = -65.0f);
    HOSTILE(ref, "weight out of range", g->candidate->model.w[7] = 1e30f);
    HOSTILE(ref, "weight out of range", g->serving->model.b[2] = -INFINITY);

    PATCHED(ref, l.candidate, 2, 1, "invalid flag");
    PATCHED(ref, l.serving, 0xff, 1, "invalid flag");
    HOSTILE(ref, "counter out of range", g->candidate->samples = top + 1);
    HOSTILE(ref, "counter out of range", g->candidate->samples = (uint64_t)1 << 63);
    HOSTILE(ref, "counter out of range", g->serving->audits = top + 1);
    HOSTILE(ref, "counter out of range", g->serving->audits = ~0ull);
    HOSTILE(ref, "non-finite number", g->candidate->regions[1].logw[2] = NAN);
    HOSTILE(ref, "non-finite number", g->serving->regions[40].logw[0] = INFINITY);
    HOSTILE(ref, "non-finite number", g->serving->drift[1].logw[1] = -INFINITY);
    HOSTILE(score, "non-finite number", g->serving->regions[31].logw[3] = -NAN);

    /* What gateCheckInvariants() says about generations. */
    HOSTILE(ref, "candidate: generation number out of range", g->candidate->gen = 0);
    HOSTILE(bare, "candidate: generation number out of range", g->candidate->gen = 8);
    HOSTILE(bare, "serving: generation number out of range", g->serving->gen = 9);
    HOSTILE(bare, "candidate and serving share a number", g->candidate->gen = 5);
    HOSTILE(ref, "candidate has a promotion time", g->candidate->promoted_ms = 1);
    HOSTILE(ref, "candidate: region counters inconsistent", g->candidate->regions[0].errors = 13);
    HOSTILE(ref, "candidate: region counters inconsistent",
            g->candidate->regions[1].certified_at = 10);
    HOSTILE(ref, "candidate: region counters inconsistent", g->candidate->regions[0].n = 13);
    HOSTILE(ref, "candidate: regions are not nested", g->candidate->regions[2].n = 10);
    HOSTILE(ref, "candidate: deferred row 2 has proof", g->candidate->regions[64].n = 1);
    HOSTILE(ref, "candidate: errors exceed samples", g->candidate->errors = 13);
    HOSTILE(ref, "candidate: audit errors exceed audits", g->candidate->audit_errors = 1);
    HOSTILE(ref, "serving: audit errors exceed audits", g->serving->audit_errors = 12);
    /* Nothing proven any more: the policy is computed from the regions, and
     * a generation without one does not serve. */
    HOSTILE(ref, "serving has no proven threshold",
            g->serving->regions[32 + 4].certified_at = g->serving->regions[32 + 6].certified_at =
                g->serving->regions[32 + 7].certified_at = 0);
    HOSTILE(score, "serving has no proven threshold", g->serving->regions[0].certified_at = 0);

    gateFree(ref);
    gateFree(bare);
    gateFree(score);
    gateFree(fresh);
}

/* The policy of a row is its lowest proven threshold, whatever a blob
 * would like it to be: it is not stored. The thresholds of the reference
 * classifier are 1 - (2/3) * 0.0015 ^ (t/31). */
TEST(policy_follows_the_regions) {
    gate *ref = golden_classifier_gate(1), *d;

    DECODED(d, ref, g->serving->regions[32 + 4].certified_at = 0);
    if (d != NULL) {
        CHECK_EQ_INT(d->serving->policy[1], 6);
        CHECK_NEAR(gateRowThreshold(d, 1), 1.0 - 2.0 / 3.0 * pow(0.0015, 6.0 / 31.0), 1e-14);
        gateFree(d);
    }
    DECODED(d, ref, g->serving->regions[32 + 2].certified_at = 1);
    if (d != NULL) {
        CHECK_EQ_INT(d->serving->policy[1], 2);
        CHECK_NEAR(gateRowThreshold(d, 1), 1.0 - 2.0 / 3.0 * pow(0.0015, 2.0 / 31.0), 1e-14);
        gateFree(d);
    }
    DECODED(d, ref, g->serving->regions[0].certified_at = 10);
    if (d != NULL) {
        CHECK_EQ_INT(d->serving->policy[0], 0);
        CHECK_EQ_INT(d->serving->policy[1], 4);
        CHECK_NEAR(gateRowThreshold(d, 0), 1.0 / 3.0, 1e-15);
        gateFree(d);
    }
    DECODED(d, ref, g->candidate->regions[1].certified_at = 0);
    if (d != NULL) {
        CHECK_EQ_INT(d->candidate->policy[0], -1);
        gateFree(d);
    }
    DECODED(d, ref, g->candidate->regions[0].certified_at = 12);
    if (d != NULL) {
        CHECK_EQ_INT(d->candidate->policy[0], 0);
        gateFree(d);
    }
    gateFree(ref);
}

TEST(hostile_requests) {
    gate *ref = golden_classifier_gate(1), *score = golden_score_gate();
    layout l = layout_of(ref), ls = layout_of(score);
    size_t at = request_at(ref, &l, 2) + REQUEST_NCLUES; /* request 5 has three clues */
    blob b;

    HOSTILE(ref, "requests out of order", gateFindObservation(g, 2)->seq = 0);
    HOSTILE(ref, "requests out of order", gateFindObservation(g, 2)->seq = 3);
    HOSTILE(ref, "requests out of order", gateFindObservation(g, 2)->seq = 4);
    HOSTILE(ref, "requests out of order", g->next_seq = 9);

    /* A request carries the clock value it was asked at: not after the
     * clock of the gate, and not before the clock's range (its age, which
     * the core computes, would overflow). */
    HOSTILE(ref, "request from the future", g->newest->created_ms++);
    HOSTILE(ref, "request from the future", set_all_times(g, 5); g->newest->created_ms = 6);
    HOSTILE(ref, "request time out of range", g->oldest->created_ms = -1);
    HOSTILE(ref, "request time out of range", g->oldest->created_ms = INT64_MIN);
    HOSTILE(ref, "request time out of range",
            gateFindObservation(g, 3)->created_ms = -GATE_MAX_FEEDBACK_TIMEOUT_MS);
    HOSTILE(score, "request time out of range", g->newest->created_ms = INT64_MIN + 1);
    PATCHED(score, request_at(score, &ls, 0) + REQUEST_CREATED + 7, 0x80, 1,
            "request time out of range");

    HOSTILE(ref, "invalid request flags", g->oldest->flags = 1u << 6);
    HOSTILE(ref, "invalid request flags", g->oldest->flags = 1u << 31);
    HOSTILE(ref, "invalid request flags", gateFindObservation(g, 5)->flags |= 1u << 6);
    HOSTILE(ref, "invalid request flags", gateFindObservation(g, 3)->flags |= 1u << 16);
    PATCHED(score, request_at(score, &ls, 1) + REQUEST_FLAGS + 1, 1, 1, "invalid request flags");
    HOSTILE(ref, "answered request outside the evidence queue",
            g->oldest->flags = GATE_OBS_RESOLVED; g->oldest->result = 1);
    HOSTILE(ref, "answered request outside the evidence queue",
            g->newest->flags |= GATE_OBS_RESOLVED; g->newest->result = 1);
    HOSTILE(ref, "answered live evidence left at the head",
            gateFindObservation(g, 5)->flags |= GATE_OBS_RESOLVED;
            gateFindObservation(g, 5)->result = 1; gateSparseFree(&gateFindObservation(g, 5)->x));
    HOSTILE(ref, "answered request still stores clues",
            gateFindObservation(g, 5)->flags |= GATE_OBS_RESOLVED;
            gateFindObservation(g, 5)->result = 1);

    /* A request the proof has moved on without is evidence, and has no
     * answer: its answer would have set it right and released it. */
    HOSTILE(ref, "request counted as an error is not unanswered evidence",
            g->oldest->flags = GATE_OBS_PROVISIONAL);
    HOSTILE(ref, "request counted as an error is not unanswered evidence",
            g->newest->flags |= GATE_OBS_PROVISIONAL);
    HOSTILE(ref, "request counted as an error is not unanswered evidence",
            gateFindObservation(g, 3)->flags = GATE_OBS_AUDIT | GATE_OBS_PROVISIONAL);
    HOSTILE(ref, "request counted as an error is not unanswered evidence",
            gateFindObservation(g, 6)->flags |= GATE_OBS_PROVISIONAL);
    HOSTILE(score, "request counted as an error is not unanswered evidence",
            g->newest->flags |= GATE_OBS_PROVISIONAL);
    HOSTILE(score, "request counted as an error is not unanswered evidence",
            g->oldest->flags &= ~(uint32_t)GATE_OBS_EVIDENCE);
    HOSTILE(ref, "invalid request answer", gateFindObservation(g, 3)->result = 1);
    HOSTILE(score, "invalid request answer", g->oldest->result = 0);
    PATCHED(score, request_at(score, &ls, 0) + REQUEST_RESULT, 1, 4, "invalid request answer");

    HOSTILE(ref, "invalid request answer", gateFindObservation(g, 6)->result = 3);
    HOSTILE(ref, "invalid request answer", gateFindObservation(g, 6)->result = -1);
    HOSTILE(ref, "invalid request answer", gateFindObservation(g, 5)->result = 0);
    HOSTILE(ref, "invalid request answer", gateFindObservation(g, 5)->result = -2);
    HOSTILE(score, "invalid request answer", g->newest->result = 2);

    HOSTILE(ref, "invalid request guess", gateFindObservation(g, 5)->cand_label = 3);
    HOSTILE(ref, "invalid request guess", gateFindObservation(g, 5)->cand_label = -1);
    HOSTILE(ref, "invalid request guess", gateFindObservation(g, 5)->serv_label = -2);
    HOSTILE(ref, "invalid request guess", gateFindObservation(g, 6)->serv_label = INT32_MAX);
    HOSTILE(ref, "invalid request guess", gateFindObservation(g, 3)->cand_label = 3);
    HOSTILE(ref, "invalid request guess", gateFindObservation(g, 3)->serv_gen = 8);
    HOSTILE(ref, "invalid request guess", g->oldest->cand_label = 0);
    HOSTILE(ref, "invalid request guess", g->oldest->serv_gen = 5);
    HOSTILE(ref, "invalid request guess", gateFindObservation(g, 5)->cand_gen = 8);
    HOSTILE(ref, "invalid request guess", g->generations = 6);
    HOSTILE(score, "invalid request guess", g->oldest->serv_label = 0);
    HOSTILE(score, "invalid request guess", g->oldest->cand_label = 0);

    HOSTILE(ref, "non-finite number", gateFindObservation(g, 5)->cand_score = NAN);
    HOSTILE(ref, "non-finite number", gateFindObservation(g, 5)->serv_score = INFINITY);
    HOSTILE(ref, "non-finite number", gateFindObservation(g, 3)->cand_score = -NAN);
    HOSTILE(score, "non-finite number", g->oldest->serv_score = -INFINITY);

    HOSTILE(ref, "invalid clues", gateFindObservation(g, 5)->x.idx[2] = GATE_DIM);
    HOSTILE(ref, "invalid clues", gateFindObservation(g, 5)->x.idx[1] = 5);
    HOSTILE(ref, "invalid clues", gateFindObservation(g, 5)->x.idx[1] = 4);
    HOSTILE(ref, "invalid clues", gateFindObservation(g, 5)->x.idx[0] = 0xffffffffu);
    HOSTILE(ref, "invalid clues", gateFindObservation(g, 5)->x.val[0] = 0.0f);
    HOSTILE(ref, "invalid clues", gateFindObservation(g, 5)->x.val[1] = -0.0f);
    HOSTILE(ref, "invalid clues", gateFindObservation(g, 5)->x.val[2] = NAN);
    HOSTILE(ref, "invalid clues", gateFindObservation(g, 3)->x.idx[1] = 7);
    HOSTILE(ref, "invalid clues", g->oldest->x.val[0] = INFINITY);
    HOSTILE(score, "score gate stores clues",
            set_clues(&g->oldest->x, 1, (const uint32_t[]){3}, (const float[]){1.0f}));

    /* The number of clues: more than there are buckets, or more than the
     * rest of the blob could hold (nothing is allocated for those). */
    PATCHED(ref, at, GATE_DIM + 1, 4, "too many clues");
    PATCHED(ref, at, 0xffffffffu, 4, "too many clues");
    PATCHED(ref, at, GATE_DIM, 4, "truncated");
    PATCHED(ref, at, 40, 4, "truncated");

    /* The number of requests. Nothing follows the count here ... */
    b = encode(score, 0);
    put(b.p + ls.requests + 8, 5, 4);
    REJECT(b.p, ls.requests + 12, "truncated");
    /* ... and here the counters, or the next request, are read as one. */
    put(b.p + ls.requests + 8, 0xffffffffu, 4);
    REJECT_ANY(b.p, b.n);
    put(b.p + ls.requests + 8, 4, 4);
    REJECT_ANY(b.p, b.n);
    put(b.p + ls.requests + 8, 2, 4);
    REJECT_ANY(b.p, b.n);
    put(b.p + ls.requests + 8, 0, 4);
    REJECT_ANY(b.p, b.n);
    blob_free(&b);

    gateFree(ref);
    gateFree(score);
}

/* Every combination of the six flags, on a request of the reference
 * classifier that waits alone or behind an unanswered piece of evidence,
 * for the generations of the gate or for generations that are gone; and
 * every other bit. The rules, from gate_core.h and gateCheckInvariants():
 * an answered request (16) is evidence (4 or 8) waiting for its turn; a
 * request the proof has moved on without (32) is evidence without an
 * answer; and answered evidence that is still proof for something does not
 * wait at the head of the queue, where its turn has come. */
TEST(request_flags) {
    static const char *const reasons[] = {
        "answered request outside the evidence queue",
        "request counted as an error is not unanswered evidence",
        "answered live evidence left at the head",
    };
    gate *bare = golden_classifier_gate(0);
    int accepted = 0, refused = 0;

    for (int variant = 0; variant < 3; variant++) {
        int behind = variant == 1, gone = variant == 2;

        for (uint32_t flags = 0; flags < 64; flags++) {
            int evidence = (flags & GATE_OBS_EVIDENCE) != 0;
            int answered = (flags & GATE_OBS_RESOLVED) != 0;
            int provisional = (flags & GATE_OBS_PROVISIONAL) != 0;
            int queued = evidence && !provisional;
            /* Which of the three rules the request breaks. */
            int broken[3] = {answered && !evidence, provisional && (!evidence || answered),
                             answered && queued && !behind && !gone};
            int valid = !broken[0] && !broken[1] && !broken[2];
            gate *g = clone(bare), *copy;
            char err[160] = "";
            gateObs *o;
            blob b, again;

            if (g == NULL) break;
            if (behind) {
                o = new_request(4, g->now_ms, GATE_OBS_CAND);
                o->cand_label = 1;
                o->cand_gen = 7;
                gateObsLink(g, o);
            }
            o = new_request(5, g->now_ms, flags);
            o->cand_label = 0;
            o->serv_label = 1;
            o->cand_score = 0.5;
            o->serv_score = 0.75;
            o->cand_gen = gone ? 6 : 7;
            o->serv_gen = gone ? 4 : 5;
            if (answered) o->result = 1;
            gateObsLink(g, o);
            b = encode(g, 0);
            copy = gateDecode(b.p, b.n, err, sizeof(err));
            MUST((copy != NULL) == valid);
            if (copy != NULL) {
                const gateObs *last = copy->newest;

                accepted++;
                /* The decoder rebuilt what is not stored. */
                MUST(last->flags == flags);
                MUST((copy->etail == last) == queued);
                MUST(copy->nevidence == (uint32_t)(behind + queued));
                MUST(copy->nanswered == (uint32_t)answered);
                MUST(gateUnanswered(copy) == (uint32_t)(behind + !answered));
                again = encode(copy, 0);
                MUST(blob_equal(&b, &again));
                blob_free(&again);
                exercise(copy, flags, 1);
                gateFree(copy);
            } else {
                int known = 0;

                refused++;
                for (int i = 0; i < 3; i++)
                    if (broken[i] && strcmp(err, reasons[i]) == 0) known = 1;
                MUST(known);
            }
            blob_free(&b);
            gateFree(g);
        }
    }
    /* Counted out. Alone, for generations that exist: no 16; without 32
     * either evidence bit, both or none (4 ways), with 32 one or both (3):
     * 7, times the four ways of bits 1 and 2: 28. Behind another request,
     * or for generations that are gone, 16 with evidence and without 32 is
     * fine too: 3 more, times four: 40 each. */
    CHECK_EQ_INT(accepted, 28 + 40 + 40);
    CHECK_EQ_INT(refused, 3 * 64 - 108);

    /* No other bit means anything. */
    for (uint32_t bit = 6; bit < 32; bit++) {
        HOSTILE(bare, "invalid request flags",
                gateObsLink(g, new_request(5, g->now_ms, 1u << bit)));
        HOSTILE(bare, "invalid request flags",
                gateObsLink(g, new_request(5, g->now_ms, GATE_OBS_AUDIT | 1u << bit)));
    }
    gateFree(bare);
}

TEST(hostile_counters) {
    gate *ref = golden_classifier_gate(1);
    const uint64_t top = (uint64_t)1 << 62;

    HOSTILE(ref, "request counters do not add up", g->stats.requests++);
    HOSTILE(ref, "request counters do not add up", g->stats.abstained--);
    /* What GINFO shows is shown as a signed number: it stops short of the
     * range in which that would be negative, with room to count on. Each
     * of the four request counters on its own: the other three are small
     * and the four add up, modulo 2^64 where one is "negative". */
    HOSTILE(ref, "counter out of range", g->stats.requests = top + 1; g->stats.accepted = top;
            g->stats.audited = 1; g->stats.abstained = 0);
    HOSTILE(ref, "counter out of range", g->stats.requests = 200; g->stats.accepted = -100ull);
    HOSTILE(ref, "counter out of range", g->stats.requests = 800; g->stats.audited = -100ull);
    HOSTILE(ref, "counter out of range", g->stats.requests = 700; g->stats.abstained = -100ull);
    HOSTILE(ref, "counter out of range", g->stats.requests = top; g->stats.accepted = 0;
            g->stats.audited = top + 1; g->stats.abstained = ~0ull);
    HOSTILE(ref, "counter out of range", g->stats.requests = ~0ull;
            g->stats.accepted = ~0ull - 300);
    HOSTILE(ref, "counter out of range", g->stats.missing = top + 1);
    HOSTILE(ref, "counter out of range", g->stats.missing = (uint64_t)1 << 63);
    HOSTILE(ref, "counter out of range", g->stats.revocations = top + 1);
    HOSTILE(ref, "counter out of range", g->stats.revocations = ~0ull);
    HOSTILE(ref, "coverage out of range", g->stats.window = GATE_COVERAGE_WINDOW + 1);
    HOSTILE(ref, "coverage out of range", g->stats.coverage = 1.0000001);
    HOSTILE(ref, "coverage out of range", g->stats.coverage = -0.25);
    HOSTILE(ref, "non-finite number", g->stats.coverage = NAN);
    gateFree(ref);
}

/* Extreme values that a gate may hold, and states the core does not leave
 * behind but copes with. */
TEST(unusual_but_valid) {
    gate *ref = golden_classifier_gate(1), *score = golden_score_gate();
    gate *blind = golden_blind_gate();
    const uint64_t top = (uint64_t)1 << 62;

    /* Both ends of the clock's range. */
    ACCEPTED(ref, g->now_ms = LAST_MS);
    ACCEPTED(ref, set_all_times(g, LAST_MS));
    ACCEPTED(ref, set_all_times(g, 0));
    ACCEPTED(ref, g->oldest->created_ms = 0); /* as old as a request can be */
    /* Times nothing is computed from may be anything. */
    ACCEPTED(ref, g->created_ms = INT64_MIN; g->candidate->frozen_ms = -1;
             g->serving->frozen_ms = INT64_MAX; g->serving->promoted_ms = INT64_MIN);
    ACCEPTED(ref, g->lineage = g->epoch = ~0ull);
    ACCEPTED(ref, g->generations = top; g->next_seq = top);
    ACCEPTED(ref, g->candidate->samples = g->serving->audits = top);
    /* The same bound for what clients are shown. */
    ACCEPTED(ref, g->trainer.updates = top; g->snapshot_updates = 5);
    ACCEPTED(ref, g->candidate->model.updates = g->serving->model.updates = top);
    ACCEPTED(ref, g->stats.requests = top; g->stats.accepted = top - 300);
    ACCEPTED(ref, g->stats.requests = top; g->stats.accepted = 0; g->stats.audited = top;
             g->stats.abstained = 0);
    ACCEPTED(ref, g->stats.requests = top; g->stats.accepted = g->stats.audited = 0;
             g->stats.abstained = top);
    ACCEPTED(ref, g->stats.missing = g->stats.revocations = top);
    /* Counters nothing is compared with and nobody is shown may hold
     * anything. */
    ACCEPTED(ref, g->candidate->budget = ~0ull; g->candidate->paired = ~0ull);
    ACCEPTED(ref, g->trainer.updates = 0; g->snapshot_updates = ~0ull);
    ACCEPTED(ref, g->serving->drift[1].n = 0; g->serving->drift[1].errors = ~0ull);
    ACCEPTED(ref, g->stats.full = g->stats.observations = g->stats.learned = ~0ull);
    ACCEPTED(ref, g->stats.ignored = g->stats.dropped = g->stats.promotions =
             g->stats.retired = ~0ull);
    ACCEPTED(ref, g->stats.window = GATE_COVERAGE_WINDOW; g->stats.coverage = 1.0);
    ACCEPTED(ref, g->stats.window = 0; g->stats.coverage = -0.0);

    /* (gateConfigValidate() knows no largest snapshot step: the limit of
     * gate_defs.h is what gate.c lets a server be configured with.) */
    ACCEPTED(ref, g->cfg.audit = 1.0; g->cfg.delta = 0.999; g->cfg.snapshot_examples = UINT32_MAX);
    ACCEPTED(ref, g->cfg.feedback_timeout_ms = GATE_MAX_FEEDBACK_TIMEOUT_MS);
    ACCEPTED(ref, g->cfg.feedback_timeout_ms = 1);
    ACCEPTED(ref, g->cfg.alpha = 1e-300; g->cfg.class_alpha[0] = g->cfg.class_alpha[1] =
             g->cfg.class_alpha[2] = 5e-324);
    ACCEPTED(ref, g->trainer.w[0] = -64.0f; g->trainer.w[1] = -0.0f; g->trainer.b[0] = FLT_MIN);
    ACCEPTED(ref, g->candidate->regions[0].logw[0] = -DBL_MAX;
             g->serving->drift[0].logw[1] = DBL_MAX);
    ACCEPTED(ref, gateFindObservation(g, 5)->cand_score = -0.0;
             gateFindObservation(g, 5)->serv_score = 1e300);
    ACCEPTED(ref, gateFindObservation(g, 5)->x.val[0] = FLT_MAX;
             gateFindObservation(g, 5)->x.val[1] = -FLT_MIN;
             gateFindObservation(g, 5)->x.idx[0] = 0);
    ACCEPTED(score, g->oldest->serv_score = -1.5; g->newest->result = 0);

    /* The run of missing answers one short of the alarm; none at all. */
    ACCEPTED(score, g->missing_run = GATE_MISSING_RUN - 1);
    ACCEPTED(ref, g->missing_run = 0);
    /* A blind gate that remembers a run, is paused, or holds nothing. */
    ACCEPTED(blind, g->missing_run = GATE_MISSING_RUN - 1);
    ACCEPTED(blind, g->paused = 1);
    ACCEPTED(blind, g->generations = 0; g->oldest->flags = 0; g->oldest->cand_gen = 0);
    /* A gate that holds nothing at all without being blind: a score gate
     * whose snapshot could not be allocated starts like that. */
    ACCEPTED(blind, g->blind = 0);

    /* Requests where the core does not leave them. Evidence for a
     * generation that is gone, answered or not ... */
    ACCEPTED(score, g->oldest->next->serv_gen = 2; g->newest->serv_gen = 1);
    ACCEPTED(score, g->oldest->serv_gen = 1);
    /* ... in a blind gate, which holds no generation at all ... */
    ACCEPTED(blind, g->newest->flags = GATE_OBS_CAND; g->newest->cand_gen = 4);
    ACCEPTED(blind, g->newest->flags = GATE_OBS_SERV | GATE_OBS_RESOLVED; g->newest->serv_gen = 3;
             g->newest->result = 0);
    /* ... a request the proof has moved on without that is newer than one
     * it still waits for, or newer than an answered one ... */
    ACCEPTED(score, g->oldest->flags &= ~(uint32_t)GATE_OBS_PROVISIONAL;
             g->oldest->next->flags |= GATE_OBS_PROVISIONAL);
    ACCEPTED(ref, gateFindObservation(g, 6)->flags = GATE_OBS_CAND | GATE_OBS_PROVISIONAL;
             gateFindObservation(g, 6)->result = -1);
    /* ... every waiting request passed by, or none ... */
    ACCEPTED(score, g->oldest->next->flags |= GATE_OBS_PROVISIONAL;
             g->newest->flags = GATE_OBS_SERV | GATE_OBS_PROVISIONAL; g->newest->result = -1);
    ACCEPTED(ref, gateFindObservation(g, 3)->flags &= ~(uint32_t)GATE_OBS_PROVISIONAL);
    /* ... proof flags without the generation they name, and a serving
     * guess of a deferred label that claims to be proof. */
    ACCEPTED(score, g->oldest->flags |= GATE_OBS_CAND);
    ACCEPTED(ref, gateFindObservation(g, 3)->flags |= GATE_OBS_SERV);
    ACCEPTED(ref, gateFindObservation(g, 5)->cand_label = 2);
    /* A candidate that has a proven threshold while nothing serves. */
    ACCEPTED(score, g->candidate = g->serving; g->serving = NULL; g->candidate->promoted_ms = 0;
             g->candidate->audits = g->candidate->audit_errors = 0);

    gateFree(ref);
    gateFree(score);
    gateFree(blind);
}

/* The reason goes into whatever room there is for it, or nowhere. */
TEST(error_buffer) {
    gate *ref = golden_score_gate();
    blob b = encode(ref, 0);
    char *one = malloc(1), *five = malloc(5);
    char same[16] = "left alone";
    gate *g;

    CHECK(gateDecode(b.p, b.n - 1, NULL, 0) == NULL);
    CHECK(gateDecode(b.p, b.n - 1, NULL, 100) == NULL);
    CHECK(gateDecode(b.p, b.n - 1, same, 0) == NULL);
    CHECK_EQ_STR(same, "left alone");
    one[0] = 'x';
    CHECK(gateDecode(b.p, b.n - 1, one, 1) == NULL);
    CHECK_EQ_INT(one[0], '\0');
    CHECK(gateDecode(b.p, b.n - 1, five, 5) == NULL);
    CHECK_EQ_STR(five, "trun");

    /* The same for the reasons the core words. */
    b.p[16] ^= 0x80; /* the sign of alpha: gateConfigValidate() objects */
    CHECK(gateDecode(b.p, b.n, NULL, 0) == NULL);
    CHECK(gateDecode(b.p, b.n, five, 5) == NULL);
    CHECK_EQ_STR(five, "ALPH");
    b.p[16] ^= 0x80;
    b.p[layout_of(ref).counters] ^= 1; /* one more request: gateCheckInvariants() objects */
    CHECK(gateDecode(b.p, b.n, NULL, 0) == NULL);
    CHECK(gateDecode(b.p, b.n, five, 5) == NULL);
    CHECK_EQ_STR(five, "requ");
    b.p[layout_of(ref).counters] ^= 1;

    g = gateDecode(b.p, b.n, NULL, 0);
    CHECK(g != NULL);
    gateFree(g);
    free(one);
    free(five);
    blob_free(&b);
    gateFree(ref);
}

/* ------------------------------- Truncation ------------------------------ */

/* Every proper prefix of a blob is refused as truncated. Each is tried in a
 * block of exactly its size, so that a read past its end is caught by the
 * memory checkers. 'stride' thins out the prefixes that end deep inside
 * weights or regions, where one is like the next. */
static void check_truncations(gate *g, size_t stride) {
    blob b = encode(g, 0);
    layout l = layout_of(g);
    size_t tried = 0, accepted = 0, other = 0;

    for (size_t n = 0; n < b.n; n++) {
        unsigned char *prefix;
        char err[160] = "";
        gate *copy;

        if (n % stride != 0 && n >= 8 && in_bulk(g, &l, n - 8) && in_bulk(g, &l, n + 8)) continue;
        prefix = malloc(n ? n : 1);
        memcpy(prefix, b.p, n);
        copy = gateDecode(prefix, n, err, sizeof(err));
        if (copy != NULL) accepted++;
        else if (strcmp(err, "truncated") != 0) other++;
        gateFree(copy);
        free(prefix);
        tried++;
    }
    CHECK_EQ_INT(accepted, 0);
    CHECK_EQ_INT(other, 0);
    CHECK(tried >= b.n / stride);
    blob_free(&b);
    gateFree(g);
}

/* A score gate with a request the proof has moved on without, a newer one
 * it waits for and an answered one behind that. */
static gate *score_with_late_answers(void) {
    gate *g = new_gate(GATE_KIND_SCORE, 0);
    int64_t now = g->created_ms + 10 + SIM_GRACE;
    uint64_t id;

    ask_one_late(g);
    MUST(ask_score(g, now + 1, 0.5, 0.2) != 0);
    id = ask_score(g, now + 2, 0.5, 0.9);
    MUST(gateLearn(g, now + 2, g->epoch, id, 1) == 1);
    return g;
}

TEST(truncated_blobs) {
    uint64_t audited, plain;
    before was;

    check_truncations(golden_score_gate(), 1);
    check_truncations(golden_blind_gate(), 1);
    check_truncations(score_with_requests(31, 0), 1);
    check_truncations(score_with_late_answers(), 1);
    check_truncations(score_blind(&audited, &plain), 1);
    check_truncations(new_gate(GATE_KIND_CLASSIFIER, 2), 1);
    check_truncations(classifier_learning(32), 1);
    check_truncations(classifier_with_everything(33, 2), 509);
    check_truncations(classifier_blind(34, &was), 127);
    check_truncations(golden_classifier_gate(1), 8191);
}

/* --------------------------- Allocation failures ------------------------- */

static long alloc_budget = -1; /* allocations that still succeed; -1: all */
static long alloc_live = 0;    /* blocks handed out and not yet returned */
static size_t alloc_bytes = 0; /* bytes asked for */

static int alloc_denied(void) {
    if (alloc_budget < 0) return 0;
    if (alloc_budget == 0) return 1;
    alloc_budget--;
    return 0;
}

static void *test_malloc(size_t n) {
    void *p = alloc_denied() ? NULL : malloc(n);

    if (p) alloc_live++;
    alloc_bytes += n;
    return p;
}

static void *test_calloc(size_t nmemb, size_t size) {
    void *p = alloc_denied() ? NULL : calloc(nmemb, size);

    if (p) alloc_live++;
    alloc_bytes += nmemb * size;
    return p;
}

static void *test_realloc(void *old, size_t n) {
    void *p = alloc_denied() ? NULL : realloc(old, n);

    if (p && old == NULL) alloc_live++;
    alloc_bytes += n;
    return p;
}

static void test_free(void *p) {
    alloc_live--;
    free(p);
}

/* Fail the first allocation, then the second, and so on until the call goes
 * through: each failure must be reported and must leak nothing. */
static void check_allocation_failures(gate *g) {
    blob b = encode(g, 0);
    unsigned char *p = NULL;
    size_t len = 0;
    gate *copy = NULL;
    long n;

    gateSetAllocator(test_malloc, test_calloc, test_realloc, test_free);
    alloc_live = 0;
    for (n = 0; n < 1000 && copy == NULL; n++) {
        char err[160] = "";

        alloc_budget = n;
        copy = gateDecode(b.p, b.n, err, sizeof(err));
        alloc_budget = -1;
        if (copy != NULL) break;
        MUST(strcmp(err, "out of memory") == 0);
        MUST(alloc_live == 0);
    }
    CHECK(copy != NULL);
    /* The gate with its index and increments, and each waiting request. */
    CHECK(n >= 4 + (long)g->npending);
    if (copy != NULL) {
        CHECK_EQ_STR(gate_diff(g, copy), "");
        gateFree(copy);
    }
    CHECK_EQ_INT(alloc_live, 0);

    for (n = 0; n < 1000; n++) {
        p = b.p; /* anything but NULL: a failure must reset it */
        alloc_budget = n;
        if (gateEncode(g, 0, &p, &len) == GATE_OK) break;
        alloc_budget = -1;
        MUST(p == NULL);
        MUST(alloc_live == 0);
    }
    alloc_budget = -1;
    CHECK(n >= 1);
    CHECK(p != NULL && len == b.n && memcmp(p, b.p, len) == 0);
    gate_free(p);
    CHECK_EQ_INT(alloc_live, 0);
    gateSetAllocator(malloc, calloc, realloc, free);

    blob_free(&b);
    gateFree(g);
}

TEST(allocation_failures) {
    uint64_t audited, plain;
    before was;

    check_allocation_failures(golden_score_gate());
    check_allocation_failures(golden_blind_gate());
    check_allocation_failures(score_with_late_answers());
    check_allocation_failures(score_blind(&audited, &plain));
    check_allocation_failures(classifier_with_everything(41, 2));
    check_allocation_failures(classifier_blind(42, &was));
    check_allocation_failures(golden_classifier_gate(1));
}

/* A count in the blob sizes the models. A blob that claims 64 classes but
 * is too short to hold their weights must be refused before the megabytes
 * are allocated. */
TEST(no_allocation_on_credit) {
    gate *g = widest_gate();
    layout l = layout_of(g);
    blob b = encode(g, 0);
    char err[160] = "";

    gateSetAllocator(test_malloc, test_calloc, test_realloc, test_free);
    /* Everything up to the weights of the trainer. */
    alloc_bytes = 0;
    CHECK(gateDecode(b.p, l.trainer + 1 + 8, err, sizeof(err)) == NULL);
    CHECK_EQ_STR(err, "truncated");
    CHECK(alloc_bytes < 16384);
    /* Everything up to the weights of the candidate: the trainer is there
     * (2 MB), the frozen copy is not. */
    alloc_bytes = 0;
    CHECK(gateDecode(b.p, l.candidate + 1 + GENERATION_BYTES + 8, err, sizeof(err)) == NULL);
    CHECK_EQ_STR(err, "truncated");
    CHECK(alloc_bytes > (2u << 20) && alloc_bytes < (3u << 20));
    gateSetAllocator(malloc, calloc, realloc, free);

    blob_free(&b);
    gateFree(g);
}

/* Nor does the number of requests a blob claims, or holds, make the decoder
 * allocate more of them than a gate can hold: it stops at the first that
 * is one too many. */
TEST(no_requests_on_credit) {
    for (int answered = 0; answered < 2; answered++) {
        gate *g = answered ? gate_with_answers_waiting() : new_gate(GATE_KIND_SCORE, 0);
        uint32_t most = answered ? GATE_MAX_WAITING + 2 : GATE_MAX_PENDING;
        layout l = layout_of(g);
        char err[160] = "";
        blob b;

        if (!answered) ask_unanswered(g, GATE_MAX_PENDING);
        /* Thousands more, linked behind the back of the core. */
        for (int i = 0; i < 3000; i++) {
            gateObs *o = new_request(g->next_seq++, g->now_ms, GATE_OBS_CAND);

            o->cand_gen = 1;
            if (answered) {
                o->flags |= GATE_OBS_RESOLVED;
                o->result = 1;
            }
            gateObsLink(g, o);
        }
        b = encode(g, 0);
        put(b.p + l.requests + 8, 0xffffffffu, 4);
        gateSetAllocator(test_malloc, test_calloc, test_realloc, test_free);
        alloc_bytes = 0;
        alloc_live = 0;
        CHECK(gateDecode(b.p, b.n, err, sizeof(err)) == NULL);
        CHECK_EQ_STR(err, answered ? "too many answers waiting" : "too many waiting requests");
        CHECK_EQ_INT(alloc_live, 0);
        /* The gate with its index, the generation it tests, and requests. */
        CHECK(alloc_bytes > most * sizeof(gateObs));
        CHECK(alloc_bytes < 65536 + (most + 1) * sizeof(gateObs));
        gateSetAllocator(malloc, calloc, realloc, free);
        blob_free(&b);
        gateFree(g);
    }
}

/* ------------------------- Every state on the way ------------------------
 * The states above were chosen. This looks at the ones nobody chose: a
 * gate of each kind lives through a long random history (see driver) and
 * is taken through the codec after every operation, or every few for the
 * larger blobs. Whatever the core leaves behind must be consistent, decode
 * into the same gate and encode to the same bytes. */

static long fuzz_count = 200000;
static uint64_t fuzz_seed = 0;

typedef struct tally {
    long checked;
    long answered;    /* with answers waiting for an older one */
    long provisional; /* with requests the proof has moved on without */
    long gone;        /* ... for generations that are gone */
    long run;         /* in a run of missing answers */
    long blind;       /* blind */
    long recorded;    /* blind, with requests recorded meanwhile */
    long seeing;      /* no longer blind, right after having been */
    long both;        /* answering while a newer snapshot is tested */
    long full;        /* full */
} tally;

static void look_at(const gate *g, tally *t, int *was_blind) {
    waiting w = count_waiting(g);

    t->checked++;
    if (w.answered) t->answered++;
    if (w.provisional) t->provisional++;
    if (w.gone) t->gone++;
    if (g->missing_run) t->run++;
    if (g->blind) t->blind++;
    if (g->blind && w.training) t->recorded++;
    if (*was_blind && !g->blind) t->seeing++;
    if (g->candidate && g->serving) t->both++;
    if (gateFull(g)) t->full++;
    *was_blind = g->blind;
}

TEST(every_state_on_the_way) {
    static const struct {
        int kind;
        uint32_t k;
        int every; /* look at one state in this many */
    } run[] = {
        {GATE_KIND_SCORE, 0, 1},
        {GATE_KIND_CLASSIFIER, 2, 3},
        {GATE_KIND_CLASSIFIER, 3, 11},
    };
    long steps = fuzz_count / 8 < 8000 ? 8000 : fuzz_count / 8;
    tally all = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    for (size_t i = 0; i < COUNTOF(run); i++) {
        gate *g = new_gate(run[i].kind, run[i].k);
        tally t = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        int was_blind = 0, bad = 0;
        driver d;

        driver_init(&d, g, 61 + i + 1000 * fuzz_seed);
        for (long step = 0; step < steps && bad < 3; step++) {
            driver_step(&d, g);
            if (step % run[i].every) continue;
            if (!check_copy(g)) {
                bad++;
                printf("  ... after operation %ld of history %zu\n", step, i);
            }
            look_at(g, &t, &was_blind);
        }
        MUST(bad == 0);
        /* Each kind of gate got into each of the new states. */
        CHECK(t.answered > 0 && t.provisional > 0 && t.run > 0);
        CHECK(t.blind > 0 && t.recorded > 0 && t.seeing > 0);
        all.checked += t.checked;
        all.answered += t.answered;
        all.provisional += t.provisional;
        all.gone += t.gone;
        all.run += t.run;
        all.blind += t.blind;
        all.recorded += t.recorded;
        all.seeing += t.seeing;
        all.both += t.both;
        all.full += t.full;
        gateFree(g);
    }
    printf("  %ld states: %ld with answers waiting, %ld with requests passed by (%ld for "
           "nothing), %ld in a run of missing answers, %ld blind (%ld with requests), %ld "
           "seeing again, %ld with two generations\n",
           all.checked, all.answered, all.provisional, all.gone, all.run, all.blind,
           all.recorded, all.seeing, all.both);
    CHECK(all.checked >= 3 * steps / 11 && all.both > 0);
}

/* A burst of requests without answers, at chosen moments of a history: the
 * gate fills up (the first burst meets a gate that tests its first
 * generation, where every request is proof); a third of the answers arrive
 * after all, newest first, and wait; proof gives up waiting and moves on
 * past thousands of requests at once; and then their time is up. */
TEST(bursts_on_the_way) {
    gate *g = new_gate_with(GATE_KIND_SCORE, 0, 600000);
    tally t = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    int was_blind = 0, bad = 0;
    waiting w;
    driver d;

    driver_init(&d, g, 71 + 1000 * fuzz_seed);
    for (int round = 0; round < 3 && bad == 0; round++) {
        /* 5000 requests in a second, none answered: more than fit. */
        for (int i = 0; i < 5000; i++) {
            if (i % 5 == 0) d.s.now++;
            sim_ask_now(&d.s, g);
        }
        if (!check_copy(g)) bad++;
        look_at(g, &t, &was_blind);
        if (round == 0) {
            CHECK(gateFull(g));
            CHECK_EQ_INT(g->npending, GATE_MAX_PENDING);
            CHECK_EQ_INT(g->stats.full, 5000 - GATE_MAX_PENDING);
        }
        /* The answers of every third arrive, newest first. */
        for (uint64_t k = 1; k <= 4500 && k < g->next_seq; k += 3)
            gateLearn(g, d.s.now, g->epoch, g->next_seq - k, (int32_t)(k % 2));
        if (!check_copy(g)) bad++;
        look_at(g, &t, &was_blind);
        w = count_waiting(g);
        /* The first request was among them, so it was applied; the other
         * 1365 answers wait behind the second. */
        if (round == 0) CHECK(w.answered == 1365 && w.unanswered == 2730 && w.provisional == 0);
        /* A quarter of the feedback timeout later proof moves on. */
        d.s.now += 150000;
        gateMaintain(g, d.s.now);
        if (!check_copy(g)) bad++;
        look_at(g, &t, &was_blind);
        w = count_waiting(g);
        if (round == 0) CHECK(w.answered == 0 && w.unanswered == 0 && w.provisional == 2730);
        if (round == 0) CHECK_EQ_INT(g->nevidence, 0);
        /* And then the answers that never came are given up: more than
         * enough of them in a row to leave the gate blind. */
        d.s.now += 450000;
        gateMaintain(g, d.s.now);
        if (!check_copy(g)) bad++;
        look_at(g, &t, &was_blind);
        if (round == 0) CHECK(g->blind && g->npending == 0);
        for (int step = 0; step < 300; step++) driver_step(&d, g);
        if (!check_copy(g)) bad++;
    }
    MUST(bad == 0);
    CHECK(t.full > 0 && t.answered > 0 && t.provisional > 0 && t.blind > 0);
    gateFree(g);
}

/* ---------------------------- Random corruption -------------------------- */

static long fuzz_accepted = 0, fuzz_rejected = 0;

/* Field-sized runs that numbers tend to be wrong in: extremes, infinities
 * and NaN as a double and as two floats, a weight just over the limit, one
 * bucket too many, the bound of the counters and one more, and as two
 * 32-bit numbers: the longest run of missing answers and one more, which
 * is also the flag of a request the proof has moved on without, alone and
 * with an answer. */
static const uint64_t fuzz_runs[] = {
    0, ~0ull, 1, 0x7fffffffffffffffull, 0x8000000000000000ull, 0xffffffff00000000ull,
    0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000000ull,
    0x7f8000007f800000ull, 0x7fc000007fc00000ull, 0x4280000142800001ull,
    0x0000200000002000ull, 0x4000000000000000ull, 0x4000000000000001ull,
    0x0000001f0000001full, 0x0000002000000020ull, 0x0000003000000030ull,
    0x0000002400000024ull, 0x0000001400000014ull,
};

/* 'count' corruptions of the encoding of 'g': one byte, one bit, a few
 * bytes anywhere, a run from the list above, or a small number. Each must
 * be refused with a reason, or give a gate that encodes back to exactly
 * the corrupted bytes and can be used. Releases 'g'. */
static void corrupt(gate *g, long count, uint64_t seed) {
    blob b = encode(g, 0);
    layout l = layout_of(g);
    unsigned char *buf = malloc(b.n); /* exactly as long: overreads show */
    uint64_t rng = seed + 1000 * fuzz_seed;

    memcpy(buf, b.p, b.n);
    for (long i = 0; i < count; i++) {
        uint64_t kind = gate_test_rand64(&rng) % 8, r = gate_test_rand64(&rng);
        uint64_t run = kind == 6 ? fuzz_runs[(r >> 8) % COUNTOF(fuzz_runs)] : (r >> 8) % 70;
        int touches = kind < 5 ? 1 : kind == 5 ? 2 + (int)(r % 7) : r % 2 ? 8 : 4;
        size_t at[8];
        unsigned char old[8];
        char err[160] = "";
        gate *copy;
        int n = 0;

        for (int j = 0; j < touches; j++) {
            size_t pos = gate_test_rand64(&rng) % b.n;
            unsigned char byte;

            /* Mostly outside the weights: they are most of a classifier
             * blob and the least interesting part of it. */
            for (int tries = 0; tries < 4 && in_weights(g, &l, pos); tries++)
                pos = gate_test_rand64(&rng) % b.n;
            if (kind >= 6 && j > 0) pos = at[0] + (size_t)j;
            if (pos >= b.n) break;
            if (kind >= 6) {
                byte = (unsigned char)(run >> (8 * j));
            } else if (kind >= 3) {
                byte = buf[pos] ^ (unsigned char)(1 + gate_test_rand64(&rng) % 255);
            } else {
                byte = buf[pos] ^ (unsigned char)(1u << (r >> 8) % 8);
            }
            at[n] = pos;
            old[n++] = buf[pos];
            buf[pos] = byte;
        }

        copy = gateDecode(buf, b.n, err, sizeof(err));
        if (copy == NULL) {
            MUST(err[0] != '\0');
            fuzz_rejected++;
        } else {
            blob again = encode(copy, 0);

            MUST(again.n == b.n && memcmp(again.p, buf, b.n) == 0);
            blob_free(&again);
            exercise(copy, r, fuzz_accepted % 8 == 0);
            gateFree(copy);
            fuzz_accepted++;
        }
        while (n > 0) {
            n--;
            buf[at[n]] = old[n];
        }
    }
    MUST(memcmp(buf, b.p, b.n) == 0);
    free(buf);
    blob_free(&b);
    gateFree(g);
}

/* A score gate that was blind until a moment ago: it tests a new
 * generation, and a request of the blind time still waits. */
static gate *score_seeing_again(void) {
    uint64_t audited, plain;
    gate *g = score_blind(&audited, &plain);

    MUST(gateLearn(g, g->now_ms + 5, g->epoch, plain, 1) == 1);
    MUST(ask_score(g, g->now_ms + 1, 0.9, 0.9) != 0);
    return g;
}

/* A score gate with a hundred requests that nobody is going to answer:
 * whoever uses it sees it go blind. */
static gate *score_about_to_go_blind(void) {
    gate *g = new_gate(GATE_KIND_SCORE, 0);

    ask_unanswered(g, 100);
    return g;
}

/* A classifier with everything, and a request the proof has moved on
 * without on top. */
static gate *classifier_with_late_answers(uint64_t seed) {
    gate *g = classifier_with_everything(seed, 2);
    sim s = {seed + 1000, g->now_ms, 0};

    leave_provisional(&s, g);
    leave_waiting(&s, g);
    return g;
}

TEST(random_corruptions) {
    uint64_t audited, plain;
    before was;

    corrupt(golden_score_gate(), fuzz_count * 12 / 100, 51);
    corrupt(score_about_to_go_blind(), fuzz_count * 4 / 100, 62);
    corrupt(golden_blind_gate(), fuzz_count * 10 / 100, 52);
    corrupt(score_with_requests(52, 0), fuzz_count * 12 / 100, 53);
    corrupt(score_with_requests(53, 1), fuzz_count * 12 / 100, 54);
    corrupt(score_with_late_answers(), fuzz_count * 14 / 100, 55);
    corrupt(score_blind(&audited, &plain), fuzz_count * 8 / 100, 56);
    corrupt(score_seeing_again(), fuzz_count * 8 / 100, 57);
    corrupt(classifier_learning(54), fuzz_count * 6 / 100, 58);
    corrupt(classifier_with_late_answers(55), fuzz_count * 6 / 100, 59);
    corrupt(classifier_blind(56, &was), fuzz_count * 4 / 100, 60);
    corrupt(golden_classifier_gate(1), fuzz_count * 4 / 100, 61);
    printf("  %ld corruptions: %ld refused, %ld decoded\n", fuzz_accepted + fuzz_rejected,
           fuzz_rejected, fuzz_accepted);
    CHECK(fuzz_accepted + fuzz_rejected >= fuzz_count * 99 / 100);
    /* Both happen a lot: a corruption that goes unnoticed (a counter, a
     * weight) and one that is caught. */
    CHECK(fuzz_accepted * 10 > fuzz_count && fuzz_rejected * 10 > fuzz_count);
}

int main(int argc, char **argv) {
    if (argc > 1) fuzz_count = strtol(argv[1], NULL, 10);
    if (argc > 2) fuzz_seed = strtoull(argv[2], NULL, 10);
    if (fuzz_count < 100) fuzz_count = 100;

    RUN(reference_score_gate);
    RUN(reference_classifier_gate);
    RUN(reference_blind_gate);
    RUN(reference_digests);
    RUN(digest_fields);
    RUN(runtime_fields_are_not_encoded);

    RUN(score_fresh);
    RUN(score_calibrating);
    RUN(score_active);
    RUN(score_paused);
    RUN(score_after_drift);
    RUN(score_after_reset);
    RUN(score_after_recover);
    RUN(score_recovered_into_the_past);
    RUN(classifier_fresh);
    RUN(classifier_learning);
    RUN(classifier_calibrating);
    RUN(classifier_active);
    RUN(classifier_background_candidate);
    RUN(classifier_paused);
    RUN(classifier_after_drift);
    RUN(classifier_after_reset);
    RUN(classifier_after_recover);
    RUN(classifier_three_classes);

    RUN(score_answers_waiting);
    RUN(score_provisional);
    RUN(score_provisional_for_nothing);
    RUN(classifier_provisional);
    RUN(score_blind);
    RUN(score_after_blind);
    RUN(classifier_blind);
    RUN(classifier_after_blind);
    RUN(blind_and_paused);

    RUN(widest_classifier);
    RUN(full_of_requests);
    RUN(full_of_clues);
    RUN(full_of_answers);
    RUN(full_of_both);
    RUN(full_again);

    RUN(hostile_header);
    RUN(previous_layout);
    RUN(truth_values);
    RUN(hostile_config);
    RUN(hostile_ledger_and_models);
    RUN(policy_follows_the_regions);
    RUN(hostile_requests);
    RUN(request_flags);
    RUN(hostile_counters);
    RUN(unusual_but_valid);
    RUN(error_buffer);
    RUN(truncated_blobs);
    RUN(allocation_failures);
    RUN(no_allocation_on_credit);
    RUN(no_requests_on_credit);
    RUN(every_state_on_the_way);
    RUN(bursts_on_the_way);
    RUN(random_corruptions);
    return TEST_REPORT();
}
