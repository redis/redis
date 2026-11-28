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
 * requests the proof has moved on without, spot checks that wait, that
 * disagreed and that were lost, a candidate compared with the generation
 * that answers, gates that went blind and gates that can see again, gates
 * at the end of their request numbers, their snapshot numbers, their
 * counters and their clock. One test takes a gate through the codec after
 * every operation of a long random history (every few for the larger
 * blobs): whatever the core can leave behind must decode.
 * Then the tests attack the decoder: hostile blobs made by hand, every
 * truncation, failing allocations, random corruption of the bytes of a
 * blob, and states made up in memory that the core would never leave
 * behind and that the decoder lets in all the same.
 *
 * THE CLOSURE PROPERTY. Whatever blob decodes, and whatever the core then
 * does with the gate, the result is consistent (gateCheckInvariants),
 * encodes, decodes again and encodes to the same bytes (and to the same
 * digest: a replica that loads the gate agrees with its primary). There is
 * no exception for counters, request numbers, snapshot numbers or a clock
 * at their limits: closes() below is what every hostile blob that decodes,
 * every corruption that decodes, every made-up state that decodes and
 * every gate at a limit is held to, again and again while the core works
 * on it.
 *
 * Usage: test_codec [number of random corruptions [seed]]. The default is
 * 200000 corruptions with seed 0; a smaller number keeps a run under
 * valgrind short, another seed tries other corruptions. The length of the
 * random histories follows the number of corruptions.
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

/* The arguments of the program: how many random corruptions, and a seed
 * for them and for the random histories. */
static long fuzz_count = 200000;
static uint64_t fuzz_seed = 0;

/* The last value of the clock (TIME in gate_core.h). */
#define LAST_MS (INT64_MAX - GATE_MAX_FEEDBACK_TIMEOUT_MS)

/* The largest value of a counter, a request number or a snapshot number
 * (CHANGES.md 9d, COUNTERS in gate_codec.c): 2^62, written out here rather
 * than taken from gate_defs.h, which the code under test includes. */
#define TOP ((uint64_t)1 << 62)

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

/* What the codec makes of a gate that was changed in memory: the decoded
 * gate, NULL (and a failure) if it does not decode. Releases 'g'. */
static gate *through_codec(gate *g) {
    gate *copy = clone(g);

    if (copy == NULL) {
        gate_test_checks++;
        gate_test_failures++;
        printf("  in %s: a gate that must decode does not\n", gate_test_current);
    }
    gateFree(g);
    return copy;
}

static void put(unsigned char *p, uint64_t v, size_t width) {
    for (size_t i = 0; i < width; i++) p[i] = (unsigned char)(v >> (8 * i));
}

static uint64_t get(const unsigned char *p, size_t width) {
    uint64_t v = 0;

    for (size_t i = 0; i < width; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

/* --------------------------------- Layout --------------------------------
 * Sizes and offsets, from the layout comment of gate_codec.c. */

#define HEADER_BYTES 8
#define LEDGER_BYTES 62     /* five 64-bit fields, the pause flag, one more,
                             * the blind flag, a 32-bit run, the answer mark */
#define GENERATION_BYTES 72 /* nine 64-bit fields */
#define REGION_BYTES 56     /* four log wealths, three counters */
#define PAIRED_BYTES 8      /* one counter for each region, after all of them */
#define DRIFT_BYTES 40      /* two counters, then three 32-bit counts of
                             * disagreements and three of lost answers */
#define REQUEST_BYTES 76    /* without its clues, eight bytes each */
#define COUNTERS_BYTES 120  /* fifteen 64-bit fields */

/* Offsets inside the ledger, inside a generation (after its presence
 * byte), a region, a drift detector, a request and the counters. */
#define LEDGER_NOW 24
#define LEDGER_GENERATIONS 32
#define LEDGER_PAUSED 40
#define LEDGER_SNAPSHOT_UPDATES 41
#define LEDGER_BLIND 49
#define LEDGER_MISSING_RUN 50
#define LEDGER_ANSWER_MARK 54
#define GENERATION_GEN 0
#define GENERATION_SAMPLES 24
#define GENERATION_ERRORS 32
#define GENERATION_MISSED 40
#define GENERATION_AUDITS 48
#define GENERATION_AUDIT_ERRORS 56
#define GENERATION_BUDGET 64
#define REGION_N 32
#define REGION_ERRORS 40
#define REGION_CERTIFIED 48
#define DRIFT_ASKED 0
#define DRIFT_ERRORS 8
#define DRIFT_WRONG 16
#define DRIFT_LOST 28
#define REQUEST_CREATED 8
#define REQUEST_FLAGS 16
#define REQUEST_CAND_GEN 44
#define REQUEST_SERV_GEN 52
#define REQUEST_RESULT 60
#define REQUEST_SPOT 64
#define REQUEST_NCLUES 72
#define COUNTERS_REQUESTS 0
#define COUNTERS_ACCEPTED 8
#define COUNTERS_AUDITED 16
#define COUNTERS_ABSTAINED 24
#define COUNTERS_WINDOW 40
#define COUNTERS_COVERAGE 48
#define COUNTERS_OBSERVATIONS 56

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
           (size_t)g->nrows * (GATE_THRESHOLDS * (REGION_BYTES + PAIRED_BYTES) + DRIFT_BYTES);
}

/* Inside a generation whose presence byte is at 'at': a region, an entry
 * of the paired table (they follow all the regions) and the drift detector
 * of a row (they follow the table). */
static size_t region_at(const gate *g, size_t at, uint32_t i) {
    return at + 1 + GENERATION_BYTES + model_bytes(g) + (size_t)i * REGION_BYTES;
}

static size_t paired_at(const gate *g, size_t at, uint32_t i) {
    return region_at(g, at, g->nrows * GATE_THRESHOLDS) + (size_t)i * PAIRED_BYTES;
}

static size_t drift_at(const gate *g, size_t at, uint32_t row) {
    return paired_at(g, at, g->nrows * GATE_THRESHOLDS) + (size_t)row * DRIFT_BYTES;
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

/* The same for the regions, the paired table and the drift detectors of a
 * generation. */
static int in_regions(const gate *g, const layout *l, size_t at) {
    size_t bytes = (size_t)g->nrows *
                   (GATE_THRESHOLDS * (REGION_BYTES + PAIRED_BYTES) + DRIFT_BYTES);
    size_t from = l->candidate + 1 + GENERATION_BYTES + model_bytes(g);

    if (g->candidate && at >= from && at < from + bytes) return 1;
    from = l->serving + 1 + GENERATION_BYTES + model_bytes(g);
    return g->serving && at >= from && at < from + bytes;
}

/* Where every counter of a blob is: each 64-bit field of the layout that
 * counts something (the number of snapshots, of examples, of samples, of
 * spot checks ...; the next request number; the number of a generation and
 * of a spot check). Not ids and times, and not the fields that name a
 * request or the generations it was asked under: those are held to their
 * own rules. Returns how many there are. */
#define MAX_COUNTERS 20000

static size_t counter_offsets(const gate *g, const layout *l, size_t *at) {
    static const size_t in_generation[] = {
        GENERATION_GEN, GENERATION_SAMPLES, GENERATION_ERRORS, GENERATION_MISSED,
        GENERATION_AUDITS, GENERATION_AUDIT_ERRORS, GENERATION_BUDGET,
    };
    size_t n = 0;
    uint32_t i;

    at[n++] = l->ledger + LEDGER_GENERATIONS;
    at[n++] = l->ledger + LEDGER_SNAPSHOT_UPDATES;
    at[n++] = l->ledger + LEDGER_ANSWER_MARK;
    if (model_bytes(g)) at[n++] = l->trainer + 1; /* examples learned from */
    for (int serving = 0; serving < 2; serving++) {
        size_t base = serving ? l->serving : l->candidate;

        if ((serving ? g->serving : g->candidate) == NULL) continue;
        for (i = 0; i < COUNTOF(in_generation); i++) at[n++] = base + 1 + in_generation[i];
        if (model_bytes(g)) at[n++] = base + 1 + GENERATION_BYTES;
        for (i = 0; i < g->nrows * GATE_THRESHOLDS; i++) {
            at[n++] = region_at(g, base, i) + REGION_N;
            at[n++] = region_at(g, base, i) + REGION_ERRORS;
            at[n++] = region_at(g, base, i) + REGION_CERTIFIED;
        }
        for (i = 0; i < g->nrows * GATE_THRESHOLDS; i++) at[n++] = paired_at(g, base, i);
        for (i = 0; i < g->nrows; i++) {
            at[n++] = drift_at(g, base, i) + DRIFT_ASKED;
            at[n++] = drift_at(g, base, i) + DRIFT_ERRORS;
        }
    }
    at[n++] = l->requests; /* the next request number */
    for (i = 0; i < g->npending; i++) at[n++] = request_at(g, l, i) + REQUEST_SPOT;
    for (size_t field = 0; field < COUNTERS_BYTES; field += 8)
        if (field != COUNTERS_COVERAGE) at[n++] = l->counters + field;
    return n;
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
    DIFF(audits);
    DIFF(audit_errors);
    DIFF(missed);
    DIFF(budget);
    if (*model_diff(&a->model, &b->model)) return model_diff(&a->model, &b->model);
    for (uint32_t i = 0; i < g->nrows * GATE_THRESHOLDS; i++) {
        for (int j = 0; j < GATE_NBETS; j++) DIFF_BITS(regions[i].logw[j]);
        DIFF(regions[i].n);
        DIFF(regions[i].errors);
        DIFF(regions[i].certified_at);
        DIFF(paired[i]);
    }
    for (uint32_t r = 0; r < g->nrows; r++) {
        DIFF(policy[r]);
        DIFF(drift[r].asked);
        DIFF(drift[r].errors);
        for (int s = 0; s < GATE_DRIFT_SCALES; s++) {
            DIFF(drift[r].wrong[s]);
            DIFF(drift[r].lost[s]);
        }
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
    DIFF(spot);
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
    DIFF(answer_mark);
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

/* 'ms' milliseconds after 'now'. The client's clock has no end: past the
 * last value of the gate's clock it goes on to INT64_MAX and stays there,
 * and the core is handed whatever it shows (it must hold it to its range). */
static int64_t later(int64_t now, int64_t ms) {
    return now > INT64_MAX - ms ? INT64_MAX : now + ms;
}

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
    s->now = later(s->now, 1 + (int64_t)(gate_test_rand64(&s->rng) % 20));
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

/* One request of a classifier with chosen guesses instead of what its
 * models say: gatePredict() takes the realized guesses as an argument, as
 * a replica does. The clues are of class 'c'. Returns its id, 0 if it was
 * not recorded. */
static uint64_t ask_guess(gate *g, int64_t now, double u, int32_t serv_label, double serv_score,
                          int32_t cand_label, double cand_score, uint32_t c)
{
    gateSparse x = {0, NULL, NULL};
    gatePredictResult res;
    uint64_t rng = 1000 + c;
    gateEval ev;

    gateMaintain(g, now);
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
    make_clues(&rng, c, &x);
    MUST(gatePredict(g, now, u, &ev, &x, &res) == GATE_OK);
    gateSparseFree(&x);
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

/* Of a candidate's proof requests, those the serving generation's own guess
 * put in one of its tested rows at all: the first column of the paired
 * table (the widest threshold of each row). */
static uint64_t paired_any(const gate *g, const gateGeneration *gen) {
    uint64_t n = 0;

    for (uint32_t r = 0; r < g->nrows; r++) n += gen->paired[r * GATE_THRESHOLDS];
    return n;
}

static int is_calibrating(const gate *g) { return g->candidate && !g->serving; }
static int is_audited(const gate *g) { return g->serving && g->serving->audits >= 5; }
static int had_alarm(const gate *g) { return g->stats.revocations > 0; }
static int has_background(const gate *g) {
    return g->serving && g->candidate && g->candidate->samples >= 5 &&
           paired_any(g, g->candidate) > 0;
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
    s->now = later(s->now, g->cfg.feedback_timeout_ms / GATE_GRACE_DIVISOR);
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
    s->now = later(s->now, g->cfg.feedback_timeout_ms);
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
    int spots;       /* spot checks of the generation that serves, without
                      * their answer */
    int stale;       /* spot checks of a generation that is gone, or whose
                      * proof flag was dropped */
    int before;      /* asked before the last answer arrived (below the
                      * answer mark) */
} waiting;

static waiting count_waiting(const gate *g) {
    waiting w = {0, 0, 0, 0, 0, 0, 0, 0, 0};

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
        if (o->spot && !(o->flags & GATE_OBS_RESOLVED)) {
            if ((o->flags & GATE_OBS_SERV) && g->serving && g->serving->gen == o->serv_gen)
                w.spots++;
            else
                w.stale++;
        }
        if (o->seq < g->answer_mark) w.before++;
    }
    return w;
}

#define DRIVE_IDS 64
#define DRIVE_BLIND_OPS 24

/* A client that does a bit of everything, and the same for any two gates
 * that behave the same. Most requests get their answer at once (half of
 * the spot checks do); the others later, usually oldest first, and now and
 * then never. It also sends ids that are stale or unknown, teaches (GADD),
 * pauses for a while, stops long enough for proof to move on without the
 * answers that are late, falls silent until everything has timed out,
 * loses every answer for a while (the gate goes blind and then sees
 * again), meets a teacher that turned against the gate and, rarely, resets
 * or recovers. 'seen' is a digest of every reply it got. */
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

    s->now = later(s->now, 1 + (int64_t)(r % 16));
    /* An outage ends with a while in which nothing asked before it can
     * still be answered: whatever the gate records then, it records blind
     * if enough answers went missing. */
    if (d->outage > 0 && --d->outage == DRIVE_BLIND_OPS)
        s->now = later(s->now, g->cfg.feedback_timeout_ms);

    if (op < 55 || (op < 85 && d->n == 0)) {
        asked a = sim_ask(s, g);

        reply = (uint64_t)a.res.action + 8 * (uint64_t)(a.res.label + 1) + 1024 * a.res.seq;
        if (d->outage) {
            /* The answer is lost. */
        } else if (a.res.seq && (a.res.action == GATE_ACTION_AUDIT ? r % 2 : r % 4)) {
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
        s->now = later(s->now, g->cfg.feedback_timeout_ms / GATE_GRACE_DIVISOR +
                                   (int64_t)((r >> 8) % 64));
    } else if (op == 94 && r % 8 == 0) {
        s->now = later(s->now, g->cfg.feedback_timeout_ms + (int64_t)(r % 1000));
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

/* THE CLOSURE PROPERTY, for a gate in whatever state the core has left it
 * in and wherever it came from (a blob made up by somebody else included):
 * it is consistent, it encodes, the encoding has the size the layout says,
 * it decodes, and what was decoded is consistent, encodes to the same
 * bytes and has the same digest (the gate itself may hold counters beyond
 * their limit, which are written as the limit: a replica that was sent the
 * encoding must still agree with it). Returns 1 if all of that holds.
 * Counts every failure, says what failed and 'when' the first time it
 * fails in a new way, and keeps count of the ways for closure_report(). */
static long closure_checks = 0, closure_failures = 0;

static struct {
    char what[240];
    long count;
} closure_ways[24];

static void closure_failed(const char *when, const char *what, const char *err) {
    char text[240];
    size_t i;

    closure_failures++;
    snprintf(text, sizeof(text), "%s%s%s", what, err[0] ? ": " : "", err);
    for (i = 0; i < COUNTOF(closure_ways) - 1 && closure_ways[i].count; i++)
        if (strcmp(closure_ways[i].what, text) == 0) break;
    if (closure_ways[i].count++ == 0) {
        snprintf(closure_ways[i].what, sizeof(closure_ways[i].what), "%s", text);
        printf("  NOT CLOSED %s: %s\n", when, text);
    }
}

/* Every way in which gates did not close, and how often. */
static void closure_report(void) {
    printf("the closure property: %ld checks, %ld failures\n", closure_checks, closure_failures);
    for (size_t i = 0; i < COUNTOF(closure_ways) && closure_ways[i].count; i++)
        printf("  %8ld x %s\n", closure_ways[i].count,
               i == COUNTOF(closure_ways) - 1 ? "(other ways)" : closure_ways[i].what);
}

static int closes(const gate *g, const char *when) {
    char err[160] = "";
    const char *what = NULL;
    blob b = {NULL, 0}, again = {NULL, 0};
    gate *copy = NULL;

    closure_checks++;
    if (!gateCheckInvariants(g, err, sizeof(err))) {
        what = "the gate is not consistent";
    } else if (gateEncode(g, 0, &b.p, &b.n) != GATE_OK) {
        what = "the gate does not encode";
    } else if (b.n != layout_of(g).end) {
        what = "the encoding has the wrong size";
    } else if ((copy = gateDecode(b.p, b.n, err, sizeof(err))) == NULL) {
        what = "the encoding is refused";
    } else if (!gateCheckInvariants(copy, err, sizeof(err))) {
        what = "the decoded gate is not consistent";
    } else if (gateEncode(copy, 0, &again.p, &again.n) != GATE_OK || !blob_equal(&b, &again)) {
        what = "the decoded gate encodes to other bytes";
    } else {
        blob mine = {NULL, 0}, its = {NULL, 0};

        if (gateEncode(g, 1, &mine.p, &mine.n) != GATE_OK ||
            gateEncode(copy, 1, &its.p, &its.n) != GATE_OK || !blob_equal(&mine, &its))
            what = "the decoded gate has another digest";
        blob_free(&mine);
        blob_free(&its);
    }
    if (what != NULL) closure_failed(when, what, err);
    gateFree(copy);
    blob_free(&b);
    blob_free(&again);
    return what == NULL;
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

#define GOLDEN_SCORE_BYTES 2634
#define GOLDEN_SCORE_NOW 73
#define GOLDEN_SCORE_COUNTERS 2514

static const char golden_score_text[] =
    "4741544501000100019a9999999999c93f7b14ae47e17a843f000000000000f0 "
    "3f00000000e80300008813000000000000080706050403020118171615141312 "
    "11feffffffffffffff0068e5cf8b01000003 *17 0700000006 *9 "
    "010300000000000000f040e5cf8b0100007854e5cf8b01000032000000000000 "
    "0002 *15 060000000000000001 *21 "
    "e03f000000000000f03f000000000000f83f0000000000000040320000000000 "
    "0000020000000000000029 *13 "
    "d0bf000000000000d03f000000000000e83f000000000000f43f1e *1677 08c0 "
    "*24 09000000000000000400000000000000040000000000000001 *231 "
    "4600000000000000030000000000000001000000020000000200000001000000 "
    "020000000300000009000000000000000300000005000000000000003060e5cf "
    "8b0100002b000000ffffffffffffffff *14 e83f *8 "
    "0300000000000000ffffffff3d *11 "
    "07000000000000001864e5cf8b01000009000000ffffffffffffffff *14 e43f "
    "*8 0300000000000000ffffffff *12 "
    "08000000000000000c66e5cf8b0100001b000000ffffffffffffffff *14 a03f "
    "*8 03000000000000000100000046 *11 "
    "64000000000000003c000000000000000a000000000000001e00000000000000 "
    "040000000000000064 *13 "
    "e83f28000000000000001e000000000000000200000000000000010000000000 "
    "00000300000000000000010000000000000002 *15";

#define GOLDEN_CLASSIFIER_BYTES 308474
#define GOLDEN_CLASSIFIER_NOW 129
#define GOLDEN_CLASSIFIER_COUNTERS 308354

static const char golden_classifier_text[] =
    "4741544501000100009a9999999999b93f000000000000b03f000000000000d0 "
    "3f03000000040000007370616d0300000068616d0a00000068c3a96c6c6f20e2 "
    "9c939a9999999999b93f9a9999999999a93f9a9999999999b93f000001140000 "
    "00e093040000000000887766554433221100ffeeddccbbaa990068e5cf8b0100 "
    "00404ae7cf8b01000007000000000000000128 *8 "
    "1f0000000400000000000000012d *10 3f *32762 80be0000c03f *65530 "
    "80420000003e000080c20000004001070000000000000020b6e5cf8b01 *10 "
    "0c00000000000000030000000000000005 *23 300200000000000028 *13 80bf "
    "*65530 403f *32775 "
    "bf9a9999999999b93f9a9999999999c93f333333333333d33f9a9999999999d9 "
    "3f0c0000000000000003 *21 "
    "f83f00000000000004400000000000000c400000000000001240090000000000 "
    "0000010000000000000007 *1719 02 *3551 "
    "0a000000000000000a00000000000000080000000000000003 *231 "
    "070000000000000002 *623 "
    "010500000000000000887be5cf8b010000108fe5cf8b0100005a000000000000 "
    "0004000000000000000d000000000000000b0000000000000002000000000000 "
    "00090000000000000014 *32774 40 *65538 803f *8 "
    "9a9999999999b9bf9a9999999999c9bf333333333333d3bf9a9999999999d9bf "
    "0a0000000000000001 *1783 500000000000000003 *47 4f *55 4e *55 4d "
    "*55 4c *15 32 *39 4b *55 4a *15 46 *13 "
    "224000000000000023400000000000002440000000000000254049 *15 3d "
    "*3399 06 *511 02 *39 "
    "5802000000000000090000000000000001000000020000000300000004000000 "
    "0500000006 *43 "
    "0b00000000000000060000000200000000000000e86be5cf8b01000000000000 "
    "ffffffffffffffff *32 ffffffff *8 "
    "0200000003000000a00f00000000803f000080bf03000000000000004004e6cf "
    "8b010000250000000000000002 *9 "
    "e43f000000000000e13f07000000000000000500000000000000ffffffff *8 "
    "0200000007000000fe1f00000000003f0000c03f0500000000000000b015e7cf "
    "8b0100000d00000001 *13 "
    "e83f000000000000e03f07000000000000000500000000000000ffffffff *8 "
    "030000000500000064000000ff1f00000000803f000000bf0000803e06000000 "
    "00000000b115e7cf8b0100001f0000000000000001 *9 "
    "ec3f000000000000ee3f07000000000000000500000000000000020000005702 "
    "*10 0900000000000000404ae7cf8b010000020000000200000001 *9 "
    "dc3f000000000000ef3f07000000000000000500000000000000ffffffff *8 01 "
    "*10 c00a00000000000000404ae7cf8b010000030000000100000001 *9 "
    "ea3f000000000000ed3f06000000000000000400000000000000ffffffff0504 "
    "03020100000001000000090000000000403fe803000000000000bc0200000000 "
    "00006400000000000000c80000000000000007 *8 02 *12 "
    "e03f2c01000000000000fa000000000000000400000000000000050000000000 "
    "0000060000000000000002000000000000000100000000000000030000000000 "
    "0000";

#define GOLDEN_BLIND_BYTES 474
#define GOLDEN_BLIND_NOW 73
#define GOLDEN_BLIND_COUNTERS 354

static const char golden_blind_text[] =
    "4741544501000100019a9999999999a93f9a9999999999b93f000000000000e0 "
    "3f000000000700000060ea000000000000a8a7a6a5a4a3a2a1b8b7b6b5b4b3b2 "
    "b101 *8 5c26050000000004 *16 "
    "0100000000006745230100000000000089674523010000000300000000604523 "
    "01000000b09825050000000024000000ffffffffffffffff000000000000ee3f "
    "*8 04 *15 ffffffff *13 "
    "67452301000000185826050000000001000000ffffffffffffffff *32 "
    "ffffffff *12 8867452301000000005c2605 *8 ffffffffffffffff *32 "
    "ffffffff *12 840300000000000064000000000000003200000000000000ee02 "
    "*30 bc02000000000000f401 *14 "
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
 * serves, its widest region is proven) and nothing is being tested. Seventy
 * spot checks have been asked: the second block of 64 is open, with one
 * disagreement and one lost answer in it, and the first blocks of 512 and
 * 4,096, with more. Three requests wait: a spot check the proof has moved
 * on without, a request that waits for its answer, and behind that a spot
 * check with its answer. Seven answers in a row are missing, and the last
 * answer that arrived did so when request 6 was the next to be asked. The
 * policy is set by hand: the decoder must find the same. */
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
    g->answer_mark = 6;

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
    s->paired[0] = 9;
    s->paired[1] = 4;
    s->paired[2] = 4;
    s->paired[3] = 1;
    s->drift[0] = (gateDrift){70, 3, {1, 2, 2}, {1, 2, 3}};
    s->policy[0] = 0;
    s->log_threshold = gateBar(g, 3);

    g->next_seq = 9;
    o = new_request(5, 1699999998000ll, GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_SERV |
                                        GATE_OBS_PROVISIONAL);
    o->serv_score = 0.75;
    o->serv_gen = 3;
    o->spot = 61;
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
    o->spot = 70;
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
 * generation 7 being tested. The candidate has a paired table and has
 * missed five requests; the serving generation has asked 600 spot checks
 * of its second label, so that blocks of all three lengths are open at
 * different places, each with disagreements and lost answers of its own.
 * Six requests wait: one kept for training from before any snapshot, one
 * the proof has moved on without, an unanswered piece of evidence, an
 * answered spot check behind it, one recorded while paused, and a spot
 * check of a generation that is gone, kept for training. The last answer
 * arrived when request 4 was the next to be asked. Without 'requests'
 * nothing waits. */
static gate *golden_classifier_gate(int requests) {
    static const char *const labels[] = {"spam", "ham", "h\xc3\xa9llo \xe2\x9c\x93"};
    static const size_t lens[] = {4, 3, 10};
    static const uint32_t idx2[] = {3, 4000}, idx3[] = {7, 8190}, idx5[] = {5, 100, 8191};
    static const uint32_t idx9[] = {0}, idx10[] = {9};
    static const float val2[] = {1.0f, -1.0f}, val3[] = {0.5f, 1.5f};
    static const float val5[] = {1.0f, -0.5f, 0.25f}, val9[] = {-2.0f}, val10[] = {0.75f};
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
    g->answer_mark = 4;

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
    c->missed = 5;
    c->budget = 560;
    MUST(gateModelInit(&c->model, GATE_DIM, 3) == GATE_OK);
    c->model.updates = 40;
    c->model.w[1] = -1.0f;
    c->model.w[16384] = 0.75f;
    c->model.b[2] = -0.5f;
    c->regions[0] = (gateRegion){{0.1, 0.2, 0.3, 0.4}, 12, 3, 0};
    c->regions[1] = (gateRegion){{1.5, 2.5, 3.5, 4.5}, 9, 1, 7};
    c->regions[32].n = 2;
    c->paired[0] = 10;
    c->paired[1] = 10;
    c->paired[2] = 8;
    c->paired[3] = 3;
    c->paired[32] = 7;
    c->paired[33] = 2;
    c->policy[0] = 1;
    c->log_threshold = gateBar(g, 7);

    s = g->serving = gateGenerationAllocate(g);
    s->gen = 5;
    s->frozen_ms = 1700000005000ll;
    s->promoted_ms = 1700000010000ll;
    s->samples = 90;
    s->errors = 4;
    s->missed = 13;
    s->audits = 11;
    s->audit_errors = 2;
    s->budget = 9;
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
    s->paired[32] = 6;
    s->drift[0] = (gateDrift){2, 0, {0, 0, 0}, {0, 0, 0}};
    s->drift[1] = (gateDrift){600, 9, {1, 2, 3}, {4, 5, 6}};
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
        o->spot = 599;
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
        o = new_request(10, 1700000123456ll, GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT);
        o->cand_label = 1;
        o->serv_label = 1;
        o->cand_score = 0.8125;
        o->serv_score = 0.90625;
        o->cand_gen = 6;
        o->serv_gen = 4;
        o->spot = 0x0102030405ull;
        set_clues(&o->x, 1, idx10, val10);
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
 * two recorded while blind so that there is an id to answer. The last
 * answer arrived when the second of them was the next to be asked. */
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
    g->answer_mark = 0x123456700ull;

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

    /* 8 + config 41 + ledger 62 + three presence bytes + a generation of
     * 72 + 32 * (56 + 8) + 40 + 12 + three requests of 76 + counters 120. */
    CHECK_EQ_INT(GOLDEN_SCORE_BYTES, 2634);
    if (d != NULL) {
        const gateObs *late = gateFindObservation(d, 5), *head = gateFindObservation(d, 7);
        const gateDrift *drift = &d->serving->drift[0];

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
        CHECK_EQ_INT(d->answer_mark, 6);
        CHECK(d->candidate == NULL && d->trainer.w == NULL);
        CHECK_EQ_INT(d->serving->regions[0].certified_at, 41);
        CHECK(d->serving->regions[31].logw[3] == -3.0);
        CHECK(d->serving->samples == 50 && d->serving->errors == 2 && d->serving->missed == 0);
        CHECK(d->serving->audits == 6 && d->serving->audit_errors == 1);
        CHECK(d->serving->paired[0] == 9 && d->serving->paired[1] == 4);
        CHECK(d->serving->paired[2] == 4 && d->serving->paired[3] == 1);
        CHECK(d->serving->paired[4] == 0 && d->serving->paired[31] == 0);
        CHECK(drift->asked == 70 && drift->errors == 3);
        CHECK(drift->wrong[0] == 1 && drift->wrong[1] == 2 && drift->wrong[2] == 2);
        CHECK(drift->lost[0] == 1 && drift->lost[1] == 2 && drift->lost[2] == 3);
        /* Of 70 spot checks, six are in the second block of 64, and all of
         * them in the first block of 512 and of 4,096. */
        CHECK_EQ_INT(gateDriftOpen(drift, 0), 6);
        CHECK_EQ_INT(gateDriftOpen(drift, 1), 70);
        CHECK_EQ_INT(gateDriftOpen(drift, 2), 70);
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
        CHECK(late && late->flags == (GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_SERV |
                                      GATE_OBS_PROVISIONAL));
        CHECK(late && late->serv_score == 0.75 && late->result == -1 && late->spot == 61);
        CHECK(head != NULL && head == d->ehead && head->enext == d->etail && head->spot == 0);
        CHECK(gateFindObservation(d, 8) == d->newest && d->newest == d->etail);
        CHECK_EQ_INT(d->newest->result, 1);
        CHECK_EQ_INT(d->newest->spot, 70);
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

    /* 8 + config 97 + ledger 62 + trainer 1 + 98324 + two generations of
     * 1 + 72 + 98324 + 3 * (32 * 64 + 40) + 12 + six requests of 76 with
     * nine clues of 8 + counters 120, a model being 8 + 3 * 8192 * 4 +
     * 3 * 4. */
    CHECK_EQ_INT(GOLDEN_CLASSIFIER_BYTES, 308474);
    if (d != NULL) {
        const gateObs *late = gateFindObservation(d, 3), *head = gateFindObservation(d, 5);
        const gateObs *stale = gateFindObservation(d, 10);
        const gateGeneration *c = d->candidate, *s = d->serving;

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
        CHECK_EQ_INT(d->answer_mark, 4);
        CHECK_EQ_INT(d->trainer.updates, 45);
        CHECK(d->trainer.w[8191] == -0.25f && d->trainer.w[8192] == 1.5f);
        CHECK(d->trainer.w[24575] == 64.0f && d->trainer.b[1] == -64.0f);
        CHECK(c->model.w[16384] == 0.75f && c->model.b[2] == -0.5f);
        CHECK(s->model.w[8191] == 2.0f && s->model.b[0] == 1.0f);
        /* The nine counters of each generation, each with its own value. */
        CHECK(c->gen == 7 && c->samples == 12 && c->errors == 3 && c->missed == 5);
        CHECK(c->audits == 0 && c->audit_errors == 0 && c->budget == 560);
        CHECK(s->gen == 5 && s->samples == 90 && s->errors == 4 && s->missed == 13);
        CHECK(s->audits == 11 && s->audit_errors == 2 && s->budget == 9);
        /* The paired table: one row of 32 counters for each label, like
         * the regions. */
        CHECK(c->paired[0] == 10 && c->paired[1] == 10 && c->paired[2] == 8 && c->paired[3] == 3);
        CHECK(c->paired[4] == 0 && c->paired[31] == 0);
        CHECK(c->paired[32] == 7 && c->paired[33] == 2 && c->paired[34] == 0);
        CHECK(c->paired[64] == 0 && c->paired[95] == 0);
        CHECK(s->paired[0] == 0 && s->paired[32] == 6 && s->paired[33] == 0);
        CHECK_EQ_INT(s->regions[32 + 7].n, 73);
        /* The drift detectors, one for each label. Of the 600 spot checks
         * of the second label, 24 are in the block of 64 now open (the
         * tenth), 88 in the second block of 512 and all in the first of
         * 4,096. */
        CHECK(s->drift[0].asked == 2 && s->drift[0].errors == 0 && s->drift[0].wrong[2] == 0);
        CHECK(s->drift[1].asked == 600 && s->drift[1].errors == 9);
        CHECK(s->drift[1].wrong[0] == 1 && s->drift[1].wrong[1] == 2 && s->drift[1].wrong[2] == 3);
        CHECK(s->drift[1].lost[0] == 4 && s->drift[1].lost[1] == 5 && s->drift[1].lost[2] == 6);
        CHECK(s->drift[2].asked == 0 && s->drift[2].lost[2] == 0);
        CHECK(c->drift[1].asked == 0 && c->drift[1].wrong[0] == 0);
        CHECK_EQ_INT(gateDriftOpen(&s->drift[1], 0), 24);
        CHECK_EQ_INT(gateDriftOpen(&s->drift[1], 1), 88);
        CHECK_EQ_INT(gateDriftOpen(&s->drift[1], 2), 600);
        CHECK_EQ_INT(d->npending, 6);
        CHECK_EQ_INT(d->pending_bytes, 6 * sizeof(gateObs) + 9 * 8);
        CHECK_EQ_INT(d->newest->seq, 10);
        CHECK(d->newest->prev->seq == 9 && d->newest->prev->x.val[0] == -2.0f);
        /* The spot check of a generation that is gone: its number means
         * nothing to the detectors of today, and stays what it was. */
        CHECK(stale != NULL && stale == d->newest && stale->spot == 0x0102030405ull);
        CHECK(stale && stale->flags == (GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT));
        CHECK(stale && stale->cand_gen == 6 && stale->serv_gen == 4 && stale->result == -1);
        CHECK(stale && stale->cand_label == 1 && stale->serv_label == 1);
        CHECK(stale && stale->cand_score == 0.8125 && stale->serv_score == 0.90625);
        CHECK(stale && stale->x.n == 1 && stale->x.idx[0] == 9 && stale->x.val[0] == 0.75f);
        CHECK_EQ_INT(gateFindObservation(d, 6)->spot, 599);
        CHECK(late && head && late->spot == 0 && head->spot == 0);

        /* What is not stored. The queue of evidence is the unanswered
         * request and the answered one behind it; the request the proof
         * has moved on without keeps its clues for the day its label
         * arrives, and is not in the queue. */
        CHECK_EQ_INT(d->nevidence, 2);
        CHECK_EQ_INT(d->nanswered, 1);
        CHECK_EQ_INT(gateUnanswered(d), 5);
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

    /* 8 + config 41 + ledger 62 + three presence bytes + 12 + three
     * requests of 76 + counters 120. */
    CHECK_EQ_INT(GOLDEN_BLIND_BYTES, 474);
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
        /* The last answer arrived between the first and the second of the
         * waiting requests. */
        CHECK(d->answer_mark == 0x123456700ull);
        CHECK_EQ_INT(count_waiting(d).before, 1);
        CHECK_EQ_INT(d->npending, 3);
        CHECK(late && late->seq == 0x123456000ull && late->cand_gen == 4 && late->spot == 0);
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
    DIGEST_CASE(ref, 0, g->oldest->created_ms++);
    DIGEST_CASE(ref, 0, g->oldest->x.val[1] = 3.0f);
    DIGEST_CASE(ref, 0, g->newest->flags |= GATE_OBS_CAND);
    DIGEST_CASE(ref, 0, g->newest->serv_score = 0.5);

    /* What the gate knows about late and lost answers is replicated: a
     * replica passes the same requests by and goes blind with its primary. */
    DIGEST_CASE(ref, 0, g->missing_run = 30);
    DIGEST_CASE(ref, 0, g->answer_mark++);
    DIGEST_CASE(ref, 0, g->answer_mark = 0);
    DIGEST_CASE(ref, 0, gateFindObservation(g, 3)->flags &= ~(uint32_t)GATE_OBS_PROVISIONAL);
    DIGEST_CASE(ref, 0, gateFindObservation(g, 5)->flags |= GATE_OBS_PROVISIONAL);
    DIGEST_CASE(blind, 0, g->blind = 0);
    DIGEST_CASE(blind, 0, g->missing_run = 1);
    DIGEST_CASE(blind, 0, g->answer_mark--);
    DIGEST_CASE(blind, 1, g->now_ms++);
    DIGEST_CASE(blind, 1, g->stats.abstained++; g->stats.requests++);

    /* So is what it knows about its spot checks and about the generation
     * it compares the candidate with: a replica raises the same alarms and
     * promotes the same snapshot at the same request. */
    DIGEST_CASE(ref, 0, g->candidate->missed++);
    DIGEST_CASE(ref, 0, g->serving->missed = 0);
    DIGEST_CASE(ref, 0, g->candidate->paired[3]++);
    DIGEST_CASE(ref, 0, g->candidate->paired[95] = 1);
    DIGEST_CASE(ref, 0, g->serving->paired[32] = 0);
    DIGEST_CASE(ref, 0, g->serving->drift[1].asked++);
    DIGEST_CASE(ref, 0, g->serving->drift[1].errors++);
    DIGEST_CASE(ref, 0, g->serving->drift[0].asked = 0);
    DIGEST_CASE(ref, 0, g->candidate->drift[2].asked = 1);
    for (int s = 0; s < GATE_DRIFT_SCALES; s++) {
        DIGEST_CASE(ref, 0, g->serving->drift[1].wrong[s]++);
        DIGEST_CASE(ref, 0, g->serving->drift[1].lost[s]--);
        DIGEST_CASE(ref, 0, g->serving->drift[0].lost[s] = 1);
    }
    DIGEST_CASE(ref, 0, gateFindObservation(g, 6)->spot--);
    DIGEST_CASE(ref, 0, gateFindObservation(g, 10)->spot = 0);
    DIGEST_CASE(ref, 0, gateFindObservation(g, 9)->spot = 1);
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
    /* Every spot check that was asked has been answered (and was counted
     * when its answer arrived), or still waits for its answer. */
    CHECK(g->serving->audits >= 5);
    CHECK_EQ_INT(g->serving->drift[0].asked, g->serving->audits + (uint64_t)w.spots);
    CHECK_EQ_INT(g->serving->drift[0].asked, g->stats.audited);
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
     * forgotten and the request stays a wrong guess. Three answers have
     * arrived since it was asked (the mark is at request 5, it is request
     * 1), so this is a lost answer and not the beginning of an outage: it
     * is missing, and it does not count towards a run. */
    CHECK_EQ_INT(g->answer_mark, late + 4);
    copy = clone(g);
    CHECK(copy != NULL);
    if (copy != NULL) {
        gateMaintain(copy, t0 + 10 + SIM_TIMEOUT - 1);
        CHECK_EQ_INT(copy->npending, 1);
        gateMaintain(copy, t0 + 10 + SIM_TIMEOUT);
        CHECK_EQ_INT(copy->npending, 0);
        CHECK_EQ_INT(copy->stats.missing, 1);
        CHECK_EQ_INT(copy->missing_run, 0);
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

/* The answer mark in the middle of the waiting requests. Twenty requests
 * get no answer; then one is asked and answered at once (the mark is now
 * behind it: every request so far has seen an answer arrive); then twenty
 * more get no answer. When all forty are given up, the first twenty are
 * lost answers and only the last twenty are a run of missing answers:
 * short of the 32 that would leave the gate blind. A copy must know where
 * the mark is: with the mark at the beginning it would count forty and go
 * blind, with the mark at the end it would count none. */
static gate *score_with_mark_in_the_middle(void) {
    gate *g = new_gate(GATE_KIND_SCORE, 0);
    int64_t t0 = g->created_ms;

    for (int i = 1; i <= 20; i++) MUST(ask_score(g, t0 + i, 0.9, 0.9) == (uint64_t)i);
    MUST(ask_score(g, t0 + 21, 0.9, 0.9) == 21);
    MUST(gateLearn(g, t0 + 21, g->epoch, 21, 1) == 1);
    for (int i = 22; i <= 41; i++) MUST(ask_score(g, t0 + i, 0.9, 0.9) == (uint64_t)i);
    return g;
}

TEST(score_answer_mark) {
    gate *g = score_with_mark_in_the_middle(), *copy;
    int64_t t0 = g->created_ms;
    waiting w;

    w = count_waiting(g);
    CHECK_EQ_INT(g->answer_mark, 22);
    CHECK_EQ_INT(g->next_seq, 42);
    CHECK_EQ_INT(g->npending, 41);
    CHECK_EQ_INT(w.before, 21);
    CHECK(w.unanswered == 40 && w.answered == 1 && w.provisional == 0);
    CHECK_EQ_INT(g->missing_run, 0);
    CHECK(check_copy(g));

    copy = clone(g);
    CHECK(copy != NULL);
    for (int which = 0; which < 2 && copy != NULL; which++) {
        gate *x = which ? copy : g;

        /* The first twenty are given up (the last of them was asked at
         * t0 + 20); the answered one has its turn. */
        gateMaintain(x, t0 + 20 + SIM_TIMEOUT);
        CHECK_EQ_INT(x->stats.missing, 20);
        CHECK_EQ_INT(x->missing_run, 0);
        CHECK_EQ_INT(x->npending, 20);
        CHECK_EQ_INT(count_waiting(x).before, 0);
        CHECK(x->candidate && x->candidate->samples == 21 && x->candidate->errors == 20);
        /* The other twenty: a run, and not a long enough one. */
        gateMaintain(x, t0 + 41 + SIM_TIMEOUT);
        CHECK_EQ_INT(x->stats.missing, 40);
        CHECK_EQ_INT(x->missing_run, 20);
        CHECK_EQ_INT(x->blind, 0);
        CHECK_EQ_INT(x->npending, 0);
        CHECK(x->candidate && x->candidate->samples == 41 && x->candidate->errors == 40);
        CHECK(gateState(x) == GATE_STATE_CALIBRATING);
    }
    if (copy != NULL) CHECK_EQ_STR(gate_diff(g, copy), "");
    gateFree(copy);
    /* The run is under way now, and the mark where it was. */
    CHECK_EQ_INT(g->answer_mark, 22);
    check_round_trip(g, 30);
}

/* ------------------------------- Spot checks -----------------------------
 * The states of gate_core.h's DRIFT paragraph and of the drift detector of
 * gate_stats.h: spot checks are numbered when they are asked, cut into
 * blocks of 64, 512 and 4,096, and a disagreement or a lost answer counts
 * in the blocks of its spot check that are still open. */

/* A score gate that answers, by the shortest way: right guesses with a
 * score of 0.9, each answered at once, until generation 1 has proven its
 * thresholds up to 0.9 and takes over. With ALPHA 0.2 a right guess
 * multiplies the four bets by 1 + c / 4 (c = 0.1, 0.25, 0.5, 0.9), and the
 * first generation of a gate with one row must reach 32 * 2 / DELTA = 320
 * at DELTA 0.2: the average of the bets is 264.3 after 34 right guesses
 * and 322.0 after 35. Regions 0 to 11 hold a score of 0.9 (see
 * score_provisional), so the gate answers from its lowest threshold, 0:
 * every request, and those that draw an audit are spot checks. The last
 * answer arrived when request 36 was the next to be asked. */
static gate *score_answering(int64_t timeout) {
    gate *g = new_gate_with(GATE_KIND_SCORE, 0, timeout);
    int64_t t0 = g->created_ms;
    int n = 0;

    while (g->serving == NULL && n < 100) {
        uint64_t id = ask_score(g, t0 + 1 + n, 0.9, 0.9);

        MUST(id != 0 && gateLearn(g, t0 + 1 + n, g->epoch, id, 1) == 1);
        n++;
    }
    CHECK_EQ_INT(n, 35);
    CHECK(g->serving != NULL && g->candidate == NULL);
    CHECK(g->serving && g->serving->policy[0] == 0 && gateRowThreshold(g, 0) == 0.0);
    CHECK_EQ_INT(g->answer_mark, 36);
    return g;
}

/* A spot check of a score gate that answers: score 0.5 and a draw below
 * AUDIT (0.3). Returns its id. */
static uint64_t ask_spot_check(gate *g, int64_t now) {
    uint64_t id = ask_score(g, now, 0.5, 0.1);
    const gateObs *o = gateFindObservation(g, id);

    MUST(o != NULL && o->spot != 0);
    MUST(o != NULL && o->flags == (GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_SERV));
    return id;
}

/* 600 spot checks, so that blocks of all three lengths are open at
 * different places: the tenth block of 64 holds spot checks 577 to 600,
 * the second block of 512 holds 513 to 600, the first of 4,096 all of
 * them. Three answers disagree (spot checks 10, 520 and 590) and four are
 * lost (2, 5, 530 and 595), each reported after all 600 were asked: a
 * disagreement or lost answer counts in the blocks of its spot check that
 * are still open then. The last three spot checks still wait. */
static gate *score_with_spot_checks(int stage, uint64_t *id) {
    gate *g = score_answering(60000);
    int64_t t1 = g->created_ms + 1000, t2 = t1 + 30000;

    /* 597 spot checks one millisecond apart; all but seven are answered
     * at once, and their cheap results were right. */
    for (int i = 1; i <= 597; i++) {
        id[i] = ask_spot_check(g, t1 + i);
        if (i == 2 || i == 5 || i == 10 || i == 520 || i == 530 || i == 590 || i == 595) continue;
        MUST(gateLearn(g, t1 + i, g->epoch, id[i], 1) == 1);
    }
    if (stage < 2) return g;
    /* Half a minute later (proof waits a quarter of a minute for a late
     * answer) three more are asked. */
    for (int i = 598; i <= 600; i++) id[i] = ask_spot_check(g, t2);
    if (stage < 3) return g;
    /* Three of the late answers arrive, and disagree; and a minute after
     * spot check 595 was asked the other four are given up. */
    MUST(gateLearn(g, t2 + 1, g->epoch, id[10], 0) == 1);
    MUST(gateLearn(g, t2 + 1, g->epoch, id[520], 0) == 1);
    MUST(gateLearn(g, t2 + 1, g->epoch, id[590], 0) == 1);
    gateMaintain(g, t1 + 595 + 60000);
    return g;
}

TEST(score_spot_checks) {
    uint64_t id[601], first;
    gate *g = score_with_spot_checks(1, id), *copy;
    int64_t t1 = g->created_ms + 1000, t2 = t1 + 30000;
    const gateDrift *d;
    waiting w;

    first = id[1];
    CHECK_EQ_INT(first, 36);
    d = &g->serving->drift[0];
    w = count_waiting(g);
    CHECK_EQ_INT(gateFindObservation(g, id[2])->spot, 2);
    CHECK_EQ_INT(gateFindObservation(g, id[595])->spot, 595);
    CHECK_EQ_INT(d->asked, 597);
    CHECK(d->errors == 0 && d->wrong[2] == 0 && d->lost[2] == 0);
    /* An answered spot check is counted when its answer arrives, though
     * as proof it waits for its turn: everything after the second waits. */
    CHECK_EQ_INT(g->serving->audits, 590);
    CHECK_EQ_INT(g->serving->audit_errors, 0);
    CHECK_EQ_INT(g->nanswered, 589);
    CHECK_EQ_INT(g->npending, 596);
    CHECK(w.spots == 7 && w.unanswered == 7 && w.answered == 589 && w.provisional == 0);
    CHECK_EQ_INT(g->serving->samples, 35 + 1);
    CHECK(check_copy(g));

    /* Half a minute later three more are asked: proof has moved on
     * without the seven. */
    gateFree(g);
    g = score_with_spot_checks(2, id);
    d = &g->serving->drift[0];
    w = count_waiting(g);
    CHECK(w.provisional == 7 && w.unanswered == 3 && w.answered == 0 && w.spots == 10);
    CHECK_EQ_INT(g->serving->samples, 35 + 597);
    CHECK_EQ_INT(g->serving->errors, 7);
    CHECK_EQ_INT(d->asked, 600);
    CHECK(check_copy(g));

    /* Three of the late answers arrive, and disagree. Spot check 10 is in
     * blocks of 64 and of 512 that have ended; 520 in a block of 64 that
     * has ended; 590 in three open blocks. */
    CHECK_EQ_INT(gateLearn(g, t2 + 1, g->epoch, id[10], 0), 1);
    CHECK(d->wrong[0] == 0 && d->wrong[1] == 0 && d->wrong[2] == 1);
    CHECK_EQ_INT(gateLearn(g, t2 + 1, g->epoch, id[520], 0), 1);
    CHECK(d->wrong[0] == 0 && d->wrong[1] == 1 && d->wrong[2] == 2);
    CHECK_EQ_INT(gateLearn(g, t2 + 1, g->epoch, id[590], 0), 1);
    CHECK(d->wrong[0] == 1 && d->wrong[1] == 2 && d->wrong[2] == 3);
    CHECK_EQ_INT(d->errors, 3);
    CHECK_EQ_INT(g->serving->audits, 593);
    CHECK_EQ_INT(g->serving->audit_errors, 3);
    CHECK_EQ_INT(g->answer_mark, first + 600);
    CHECK(check_copy(g));

    /* A minute after spot check 595 was asked the other four are given
     * up: 2 and 5 count in the block of 4,096 only, 530 also in the block
     * of 512, 595 in all three. They were asked before the last answer
     * arrived, so they are lost answers and no run of missing ones. */
    gateMaintain(g, t1 + 595 + 60000);
    w = count_waiting(g);
    CHECK(d->lost[0] == 1 && d->lost[1] == 2 && d->lost[2] == 4);
    CHECK(d->wrong[0] == 1 && d->wrong[1] == 2 && d->wrong[2] == 3);
    CHECK(d->asked == 600 && d->errors == 3);
    CHECK_EQ_INT(gateDriftOpen(d, 0), 24);
    CHECK_EQ_INT(gateDriftOpen(d, 1), 88);
    CHECK_EQ_INT(gateDriftOpen(d, 2), 600);
    CHECK_EQ_INT(g->stats.missing, 4);
    CHECK_EQ_INT(g->missing_run, 0);
    CHECK(!g->blind && gateState(g) == GATE_STATE_ACTIVE && g->stats.revocations == 0);
    CHECK_EQ_INT(g->serving->audits, 593);
    CHECK_EQ_INT(g->serving->samples, 35 + 597);
    CHECK_EQ_INT(g->serving->errors, 7);
    /* A score of 0.5 lies in regions 0 to 3 (thresholds 1 - 0.002^(t/31):
     * 0.4520 for t = 3, 0.5515 for t = 4). */
    CHECK(g->serving->regions[3].n == 35 + 597 && g->serving->regions[3].errors == 7);
    CHECK(g->serving->regions[4].n == 35 && g->serving->regions[4].errors == 0);
    CHECK_EQ_INT(g->stats.audited, 600);
    CHECK_EQ_INT(g->stats.requests, 635);
    CHECK_EQ_INT(g->npending, 3);
    CHECK(w.spots == 3 && w.unanswered == 3 && w.provisional == 0 && w.before == 3);
    CHECK(g->oldest && g->oldest->spot == 598 && g->newest->spot == 600);
    CHECK(check_copy(g));
    /* (The same state, built in one go for the tests further down.) */
    copy = score_with_spot_checks(3, id);
    CHECK_EQ_STR(gate_diff(g, copy), "");
    gateFree(copy);

    /* A copy counts on where the original would: the answer of spot check
     * 599 disagrees, in three open blocks. */
    copy = clone(g);
    CHECK(copy != NULL);
    if (copy != NULL) {
        const gateDrift *dc = &copy->serving->drift[0];

        CHECK_EQ_INT(gateLearn(copy, t1 + 595 + 60000, copy->epoch, id[599], 0), 1);
        CHECK(dc->wrong[0] == 2 && dc->wrong[1] == 3 && dc->wrong[2] == 4 && dc->errors == 4);
        CHECK(dc->lost[0] == 1 && dc->lost[1] == 2 && dc->lost[2] == 4);
        CHECK(copy->serving->audits == 594 && copy->serving->audit_errors == 4);
        CHECK(check_copy(copy));
        gateFree(copy);
    }
    check_round_trip(g, 31);
}

/* Six disagreements in a fresh block are one short of the alarm: with
 * ALPHA 0.2 a disagreement multiplies the two likelihood ratios by 2 and
 * by 3, a right or unanswered spot check by 0.75 and by 0.5, and the alarm
 * is at 1,000: (2^6 + 3^6) / 2 = 396.5, and with a seventh spot check
 * asked and not answered (2^6 * 0.75 + 3^6 * 0.5) / 2 = 206.25; when that
 * one disagrees too it is (2^7 + 3^7) / 2 = 1157.5. A copy must raise the
 * alarm where the original does. */
static gate *score_short_of_the_alarm(void) {
    gate *g = score_answering(60000);
    int64_t t1 = g->created_ms + 1000;

    for (int i = 1; i <= 7; i++) {
        uint64_t id = ask_spot_check(g, t1 + i);

        if (i < 7) MUST(gateLearn(g, t1 + i, g->epoch, id, 0) == 1);
    }
    return g;
}

TEST(score_one_disagreement_short_of_the_alarm) {
    gate *g = score_short_of_the_alarm(), *copy;
    int64_t t1 = g->created_ms + 1000;
    const gateDrift *d = &g->serving->drift[0];
    uint64_t last = g->newest ? g->newest->seq : 0;

    CHECK(d->asked == 7 && d->errors == 6);
    CHECK(d->wrong[0] == 6 && d->wrong[1] == 6 && d->wrong[2] == 6);
    CHECK(d->lost[0] == 0 && d->lost[1] == 0 && d->lost[2] == 0);
    CHECK(g->serving->audits == 6 && g->serving->audit_errors == 6);
    CHECK(gateState(g) == GATE_STATE_ACTIVE && g->stats.revocations == 0);
    CHECK_EQ_INT(count_waiting(g).spots, 1);
    CHECK(check_copy(g));

    copy = clone(g);
    CHECK(copy != NULL);
    for (int which = 0; which < 2 && copy != NULL; which++) {
        gate *x = which ? copy : g;

        CHECK_EQ_INT(gateLearn(x, t1 + 8, x->epoch, last, 0), 1);
        /* The alarm: the generation is withdrawn, and a score gate starts
         * its next round of testing at once. */
        CHECK(x->serving == NULL && x->stats.revocations == 1 && !x->blind);
        CHECK(x->candidate != NULL && x->candidate->gen == 2 && x->generations == 2);
        CHECK_EQ_INT(x->npending, 0);
        CHECK(gateState(x) == GATE_STATE_CALIBRATING);
    }
    if (copy != NULL) CHECK_EQ_STR(gate_diff(g, copy), "");
    gateFree(copy);
    check_round_trip(g, 32);
}

/* Lost spot checks stop a gate that answers: it can no longer rule out
 * that it got worse. Counted as disagreements, 19 lost answers of 20 spot
 * checks asked give (2^19 * 0.75 + 3^19 * 0.5) / 2 = 2.9e8, short of the
 * 1e9 at which the gate gives up; the twentieth gives (2^20 + 3^20) / 2 =
 * 1.7e9. (No answer arrives meanwhile, so they are also a run of missing
 * answers: of 20, short of the 32 that would do the same.) The gate keeps
 * what it has learned and is blind until an answer arrives. */
static gate *score_short_of_blind(void) {
    gate *g = score_answering(60000);
    int64_t t1 = g->created_ms + 1000;

    for (int i = 1; i <= 19; i++) ask_spot_check(g, t1 + i);
    ask_spot_check(g, t1 + 5000);
    gateMaintain(g, t1 + 19 + 60000);
    return g;
}

TEST(score_one_lost_answer_short_of_blind) {
    gate *g = score_short_of_blind(), *copy;
    int64_t t1 = g->created_ms + 1000;
    const gateDrift *d = &g->serving->drift[0];
    waiting w;

    w = count_waiting(g);
    CHECK(d->asked == 20 && d->errors == 0);
    CHECK(d->lost[0] == 19 && d->lost[1] == 19 && d->lost[2] == 19);
    CHECK(d->wrong[0] == 0 && d->wrong[1] == 0 && d->wrong[2] == 0);
    CHECK_EQ_INT(g->stats.missing, 19);
    CHECK_EQ_INT(g->missing_run, 19);
    CHECK_EQ_INT(g->serving->audits, 0);
    CHECK(gateState(g) == GATE_STATE_ACTIVE && !g->blind && g->stats.revocations == 0);
    CHECK(w.spots == 1 && w.unanswered == 1 && w.before == 0);
    CHECK(check_copy(g));

    copy = clone(g);
    CHECK(copy != NULL);
    for (int which = 0; which < 2 && copy != NULL; which++) {
        gate *x = which ? copy : g;

        gateMaintain(x, t1 + 5000 + 60000 - 1);
        CHECK(x->serving != NULL && !x->blind);
        gateMaintain(x, t1 + 5000 + 60000);
        CHECK(x->serving == NULL && x->candidate == NULL);
        CHECK_EQ_INT(x->blind, 1);
        CHECK_EQ_INT(x->stats.revocations, 1);
        CHECK_EQ_INT(x->stats.missing, 20);
        CHECK_EQ_INT(x->missing_run, 0);
        CHECK_EQ_INT(x->npending, 0);
        CHECK(gateState(x) == GATE_STATE_COLD);
    }
    if (copy != NULL) CHECK_EQ_STR(gate_diff(g, copy), "");
    gateFree(copy);
    check_round_trip(g, 33);
}

/* --------------------------- Candidate and serving ------------------------
 * A candidate that is tested while another generation answers: the paired
 * table says what the serving generation's own guess made of each of the
 * candidate's proof requests, and 'missed' counts the requests the
 * candidate left to the teacher by guessing a label that is never
 * answered. The guesses are chosen here, not computed by the models (see
 * ask_guess): a three-label classifier of new_gate(), whose third label
 * is never answered. Its thresholds are 1 - (2/3) * 0.0015^(t/31): a score
 * of 0.4 lies in region 0 only, 0.5 in regions 0 and 1 (0.4595, then
 * 0.5618), 0.7 in the first four, 0.8 in the first six, 0.95 in the first
 * thirteen (0.9462, then 0.9564), and 0.999 in all 32. */

/* One request with chosen guesses, answered at once. */
static void guess_and_learn(gate *g, int64_t now, double u, int32_t serv_label,
                            double serv_score, int32_t cand_label, double cand_score,
                            int32_t answer)
{
    uint64_t id = ask_guess(g, now, u, serv_label, serv_score, cand_label, cand_score,
                            (uint32_t)answer);

    MUST(id != 0 && gateLearn(g, now, g->epoch, id, answer) == 1);
}

/* The classifier of new_gate() with generation 1 answering for its first
 * label and generation 2 just frozen. Twenty examples are taught (the
 * snapshot step), then generation 1 guesses the first label 39 times with
 * a score of 0.95 and is right: two labels can be answered, so it must
 * reach 32 * 2 * 2 / DELTA = 640, and the average of the four bets is
 * above that after 39 right guesses, not after 38. Every answer was also
 * an example: 59 by then, 39 since the snapshot, so the next one is taken
 * at once. Returns the gate at time created + 100. */
static gate *classifier_two_generations(void) {
    gate *g = new_gate(GATE_KIND_CLASSIFIER, 3);
    int64_t t0 = g->created_ms;
    uint64_t rng = 36;
    int n = 0;

    for (int i = 0; i < 20; i++) {
        gateSparse x;

        make_clues(&rng, (uint32_t)i % 3, &x);
        MUST(gateTrain(g, t0 + 1, &x, i % 3) == GATE_OK);
        gateSparseFree(&x);
    }
    CHECK(g->candidate != NULL && g->candidate->gen == 1 && g->serving == NULL);
    /* Ten times the proof a flawless threshold needs, for each of the two
     * labels: 39 right guesses at ALPHA 0.2, 82 at the 0.1 of the second
     * label, and the larger counts. */
    CHECK_EQ_INT(g->candidate ? g->candidate->budget : 0, 10 * 82 * 2);
    while (g->serving == NULL && n < 100) {
        guess_and_learn(g, t0 + 2 + n, 0.9, -1, 0.0, 0, 0.95, 0);
        n++;
    }
    CHECK_EQ_INT(n, 39);
    CHECK(g->serving != NULL && g->serving->gen == 1 && g->serving->policy[0] == 0);
    CHECK(g->serving && g->serving->policy[1] == -1 && g->serving->policy[2] == -1);
    CHECK(g->serving && g->serving->regions[12].certified_at == 39);
    CHECK(g->serving && g->serving->regions[13].n == 0);
    CHECK(g->candidate != NULL && g->candidate->gen == 2 && g->candidate->samples == 0);
    CHECK_EQ_INT(g->candidate ? g->candidate->model.updates : 0, 59);
    /* Generation 2 needs 44 right guesses at ALPHA 0.2 and 94 at 0.1. */
    CHECK_EQ_INT(g->candidate ? g->candidate->budget : 0, 10 * 94 * 2);
    gateMaintain(g, t0 + 100);
    return g;
}

/* Nine requests of that gate, each answered at once: the draw, the guess
 * of the serving generation, the guess of the candidate, and the
 * teacher's answer. A draw of 0.1 is an audit, one of 0.9 is not. */
static const struct {
    double u;
    int32_t serv_label;
    double serv_score;
    int32_t cand_label;
    double cand_score;
    int32_t answer;
} paired_steps[] = {
    /* 0: a spot check of the first label (the serving generation would
     * have answered: 0.5 is above its threshold of 1/3), which is also
     * proof for the candidate: paired in regions 0 and 1 of row 0. */
    {0.1, 0, 0.5, 0, 0.7, 0},
    /* 1: audited; the serving generation guessed the second label, which
     * it does not answer yet, with 0.95: paired in the first thirteen
     * regions of row 1. */
    {0.1, 1, 0.95, 0, 0.6, 0},
    /* 2: a spot check with the highest score there is: all of row 0. The
     * serving generation was wrong and the candidate right. */
    {0.1, 0, 0.999, 1, 0.8, 1},
    /* 3: audited; the serving generation guessed the label that is never
     * answered: proof for the candidate, paired with nothing. */
    {0.1, 2, 0.9, 0, 0.5, 0},
    /* 4: not audited: proof for the serving generation only (its second
     * label has no threshold yet, so every such request is). */
    {0.9, 1, 0.4, 0, 0.9, 1},
    /* 5: audited, and the candidate guessed the label that is never
     * answered although the teacher's answer is one that is: a request it
     * could have been tested on. Missed. */
    {0.1, 1, 0.6, 2, 0.9, 0},
    /* 6: the same, but the teacher's answer is that label too: not
     * missed. */
    {0.1, 1, 0.6, 2, 0.9, 2},
    /* 7: the same as 5, but not audited: while a generation answers, only
     * the audited requests are the candidate's sample. Not missed. */
    {0.9, 1, 0.5, 2, 0.9, 0},
    /* 8: a spot check again, missed by the candidate. */
    {0.1, 0, 0.4, 2, 0.7, 1},
};

static void paired_step(gate *g, int64_t now, size_t i) {
    guess_and_learn(g, now + 1 + (int64_t)i, paired_steps[i].u, paired_steps[i].serv_label,
                    paired_steps[i].serv_score, paired_steps[i].cand_label,
                    paired_steps[i].cand_score, paired_steps[i].answer);
}

/* The gate after those nine requests, with one more that waits: a spot
 * check that is proof for both generations. */
static gate *classifier_with_paired_table(void) {
    gate *g = classifier_two_generations();
    int64_t now = g->now_ms;

    for (size_t i = 0; i < COUNTOF(paired_steps); i++) paired_step(g, now, i);
    MUST(ask_guess(g, now + 10, 0.1, 0, 0.9, 0, 0.9, 0) != 0);
    return g;
}

TEST(classifier_paired_and_missed) {
    static const uint64_t samples[] = {1, 2, 3, 4, 4, 4, 4, 4, 4};
    static const uint64_t missed[] = {0, 0, 0, 0, 0, 1, 1, 1, 2};
    gate *g = classifier_two_generations(), *copy;
    int64_t now = g->now_ms;
    const gateGeneration *c = g->candidate, *s = g->serving;
    const gateObs *o;
    int bad = 0;

    for (size_t i = 0; i < COUNTOF(paired_steps) && g->candidate && g->serving; i++) {
        paired_step(g, now, i);
        /* The two generations stay what they are throughout. */
        if (g->candidate != c || g->serving != s) break;
        CHECK_EQ_INT(c->samples, samples[i]);
        CHECK_EQ_INT(c->missed, missed[i]);
        if (i == 0) CHECK(c->paired[0] == 1 && c->paired[1] == 1 && c->paired[2] == 0);
        if (i == 1) CHECK(c->paired[32] == 1 && c->paired[32 + 12] == 1 && c->paired[32 + 13] == 0);
        if (i == 2) CHECK(c->paired[0] == 2 && c->paired[1] == 2 && c->paired[2] == 1);
        if (i == 2) CHECK(c->paired[31] == 1 && c->paired[32] == 1);
    }
    CHECK(c != NULL && s != NULL && g->candidate == c && g->serving == s);
    if (c == NULL || s == NULL || g->candidate != c || g->serving != s) {
        gateFree(g);
        return;
    }

    /* The table: row 0 holds 2, 2 and thirty times 1; row 1 thirteen times
     * 1; the row of the label that is never answered nothing. */
    for (uint32_t t = 0; t < GATE_THRESHOLDS; t++) {
        if (c->paired[t] != (t < 2 ? 2u : 1u)) bad++;
        if (c->paired[32 + t] != (t < 13 ? 1u : 0u)) bad++;
        if (c->paired[64 + t] != 0) bad++;
    }
    CHECK_EQ_INT(bad, 0);
    CHECK_EQ_INT(paired_any(g, c), 3);
    CHECK(c->samples == 4 && c->errors == 0 && c->missed == 2 && c->budget == 1880);
    /* Its own guesses of the first label scored 0.7, 0.6 and 0.5 (four,
     * three and two regions), the one of the second label 0.8 (six). */
    CHECK(c->regions[1].n == 3 && c->regions[2].n == 2 && c->regions[3].n == 1);
    CHECK(c->regions[4].n == 0 && c->regions[32 + 5].n == 1 && c->regions[32 + 6].n == 0);
    /* The serving generation: 39 samples of its own test, and eight of
     * the nine requests since (not the one it guessed the third label
     * for); wrong six times. Three were spot checks, two of them wrong. */
    CHECK(s->samples == 47 && s->errors == 6 && s->audits == 3 && s->audit_errors == 2);
    CHECK(s->drift[0].asked == 3 && s->drift[0].errors == 2);
    CHECK(s->drift[0].wrong[0] == 2 && s->drift[0].wrong[1] == 2 && s->drift[0].wrong[2] == 2);
    CHECK(s->drift[1].asked == 0 && s->drift[2].asked == 0);
    CHECK(s->paired[0] == 0 && s->paired[32] == 0 && s->missed == 0);
    CHECK_EQ_INT(g->trainer.updates, 68);
    CHECK_EQ_INT(g->snapshot_updates, 59);
    CHECK(g->stats.requests == 48 && g->stats.audited == 3 && g->stats.abstained == 45);
    CHECK_EQ_INT(g->npending, 0);
    CHECK(gateState(g) == GATE_STATE_ACTIVE);
    CHECK(check_copy(g));

    /* With a spot check waiting that is proof for both. */
    gateFree(g);
    g = classifier_with_paired_table();
    o = g->newest;
    CHECK_EQ_INT(g->npending, 1);
    CHECK(o != NULL && o->spot == 4);
    CHECK(o != NULL && o->flags == (GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_EVIDENCE));
    CHECK(g->candidate != NULL && g->candidate->missed == 2 && g->candidate->paired[0] == 2);
    /* A copy promotes where the original does, and that depends on the
     * table: the candidate takes over when it has a proven threshold and
     * its policy holds more of its proof requests than the serving
     * generation's policy holds of the same requests, each judged by its
     * own guess. The spot check that waits is answered (both were right,
     * with 0.9: ten regions). Then one request for which the candidate
     * guesses the second label and the serving generation the first: the
     * first entry of the table is now 4, and the candidate's widest region
     * of the first label holds 4 too. */
    copy = clone(g);
    CHECK(copy != NULL);
    for (int which = 0; which < 2 && copy != NULL; which++) {
        gate *x = which ? copy : g;

        CHECK_EQ_INT(gateLearn(x, now + 11, x->epoch, x->newest->seq, 0), 1);
        guess_and_learn(x, now + 12, 0.1, 0, 0.9, 1, 0.6, 1);
        CHECK(x->candidate != NULL && x->candidate->gen == 2 && x->candidate->samples == 6);
        CHECK(x->candidate && x->candidate->paired[0] == 4 && x->candidate->regions[0].n == 4);
        CHECK(x->candidate && x->candidate->paired[9] == 3 && x->candidate->paired[10] == 1);
        /* Sixty requests that both guess the first label for, and are
         * right: the candidate with 0.999, the serving generation with
         * 0.9. After forty of them the candidate has proven its widest
         * threshold (44 right guesses in it, what generation 2 needs at
         * ALPHA 0.2), but its policy holds as many of its proof requests
         * as the serving generation's: it does not take over. */
        for (int k = 1; k <= 60; k++) {
            guess_and_learn(x, now + 13 + k, 0.1, 0, 0.9, 0, 0.999, 0);
            if (k == 39) CHECK(x->candidate && x->candidate->policy[0] == -1);
            if (k == 40) CHECK(x->candidate && x->candidate->policy[0] == 0);
        }
        CHECK(x->serving != NULL && x->serving->gen == 1);
        CHECK(x->candidate != NULL && x->candidate->gen == 2 && x->candidate->samples == 66);
        CHECK(x->candidate && x->candidate->paired[0] == 64 && x->candidate->regions[0].n == 64);
        CHECK(x->candidate && x->candidate->regions[0].certified_at == 44);
        CHECK_EQ_INT(x->stats.promotions, 1);
        if (which == 0) CHECK(check_copy(x));
        /* One request that the serving generation would not have answered
         * (it guesses the second label): 65 against 64. */
        guess_and_learn(x, now + 80, 0.1, 1, 0.5, 0, 0.999, 0);
        CHECK(x->serving != NULL && x->serving->gen == 2 && x->serving->samples == 67);
        CHECK(x->serving && x->serving->paired[0] == 0 && x->serving->paired[32] == 0);
        CHECK(x->candidate != NULL && x->candidate->gen == 3);
        CHECK_EQ_INT(x->stats.promotions, 2);
    }
    if (copy != NULL) CHECK_EQ_STR(gate_diff(g, copy), "");
    gateFree(copy);
    check_round_trip(g, 36);
}

/* Spot checks that outlive their generation: the drift alarm withdraws the
 * generation they were spot checks of. In a classifier they stay, for the
 * model to learn from their answers (their proof flags are dropped), and
 * keep the number they had: it means nothing to the detectors of the
 * generation that answers next, which have handed out fewer. 'stage' 1
 * stops when the three wait, 2 after the alarm. */
static gate *classifier_with_stale_spot_checks(int stage) {
    gate *g = classifier_two_generations();
    int64_t now = g->now_ms;
    uint64_t rng = 37;
    int n;

    /* Three spot checks of the first label that get no answer. */
    for (int i = 0; i < 3; i++) MUST(ask_guess(g, now + 1, 0.1, 0, 0.9, 0, 0.9, 0) != 0);
    if (stage < 2) return g;
    /* Spot checks whose answers disagree, until the alarm. */
    for (n = 0; g->stats.revocations == 0 && n < 50; n++)
        guess_and_learn(g, now + 2 + n, 0.1, 0, 0.9, 0, 0.9, 1);
    if (stage < 3) return g;
    /* The gate learns again and answers again, and asks a spot check. */
    for (int i = 0; i < 20; i++) {
        gateSparse x;

        make_clues(&rng, (uint32_t)i % 3, &x);
        MUST(gateTrain(g, now + 20, &x, i % 3) == GATE_OK);
        gateSparseFree(&x);
    }
    for (n = 0; g->serving == NULL && n < 100; n++)
        guess_and_learn(g, now + 21 + n, 0.9, -1, 0.0, 0, 0.95, 0);
    MUST(ask_guess(g, now + 80, 0.1, 0, 0.9, 0, 0.9, 0) != 0);
    return g;
}

TEST(classifier_spot_checks_of_a_generation_that_is_gone) {
    gate *g = classifier_with_stale_spot_checks(1);
    const gateObs *o;
    uint64_t n = 0;
    waiting w;

    w = count_waiting(g);
    CHECK(w.spots == 3 && w.stale == 0 && w.unanswered == 3);
    for (o = g->oldest; o; o = o->next) CHECK(o->spot == ++n);
    CHECK(check_copy(g));
    gateFree(g);

    /* Spot checks whose answers disagree, until the alarm. The three that
     * wait count as right (0.75 and 0.5 each, at the ALPHA of 0.2 of this
     * label): (2^8 * 0.75^3 + 3^8 * 0.5^3) / 2 = 464.1 after eight
     * disagreements, 1338.2 after nine. As proof the answers wait behind
     * the three; the detector hears of each when it arrives. */
    g = classifier_with_stale_spot_checks(2);
    w = count_waiting(g);
    CHECK_EQ_INT(g->stats.revocations, 1);
    CHECK_EQ_INT(g->stats.requests, 39 + 3 + 9);
    CHECK_EQ_INT(g->stats.learned, 39 + 9);
    /* The alarm of a classifier: both generations and the model go. */
    CHECK(g->serving == NULL && g->candidate == NULL && g->trainer.updates == 0);
    CHECK(gateState(g) == GATE_STATE_COLD && !g->blind);
    CHECK_EQ_INT(g->npending, 3);
    CHECK(w.spots == 0 && w.stale == 3 && w.training == 3 && w.clues == 3);
    n = 0;
    for (o = g->oldest; o; o = o->next) {
        CHECK(o->flags == (GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT));
        CHECK(o->spot == ++n && o->serv_gen == 1 && o->cand_gen == 2);
    }
    CHECK(check_copy(g));
    gateFree(g);

    /* The gate learns again: twenty examples, a snapshot (generation 3,
     * which must reach 32 * 2 * 12 / DELTA = 3840: 48 right guesses at
     * ALPHA 0.2), and it answers again while the three still wait. */
    g = classifier_with_stale_spot_checks(3);
    w = count_waiting(g);
    CHECK_EQ_INT(g->stats.learned, 39 + 9 + 48);
    CHECK(g->serving != NULL && g->serving->gen == 3 && g->serving->policy[0] == 0);
    CHECK(g->serving && g->serving->drift[0].asked == 1);
    CHECK(w.spots == 1 && w.stale == 3);
    CHECK_EQ_INT(g->npending, 4);
    CHECK(g->newest && g->newest->spot == 1 && g->newest->serv_gen == 3);
    CHECK(g->oldest && g->oldest->next && g->oldest->next->next);
    CHECK(g->oldest->next->next->spot == 3 && g->oldest->next->next->serv_gen == 1);
    check_round_trip(g, 37);
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
    /* 8 + config (41 + 64 * (4 + 64) + 64 * 9) + 62 + trainer (1 + 2097416)
     * + candidate (1 + 72 + 2097416 + 64 * (32 * 64 + 40)) + 1 + 12 + 120
     * bytes, a model being 8 + 64 * 8192 * 4 + 64 * 4 = 2097416. */
    CHECK_EQ_INT(layout_of(g).end, 4333710);
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
    /* 2406 bytes without requests (8 + 41 + 62 + 1 + 2161 + 1 + 12 + 120,
     * the generation being 1 + 72 + 32 * 64 + 40). */
    CHECK_EQ_INT(layout_of(g).end, 2406 + (GATE_MAX_WAITING + 1) * 76);
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
 * Whatever the decoder lets through is a gate: it must be safe to use, and
 * whatever the core makes of it must go through the codec again. */

/* What gate_core.h promises about a blind gate: it records what it is
 * asked while it has room, so that there is an id to answer, and the
 * answer makes it see again; a score gate starts testing in the same call
 * (unless it has used up its snapshot numbers). */
static void see_again(sim *s, gate *g) {
    asked a = sim_ask_now(s, g);

    MUST(a.res.seq != 0 || gateFull(g) || gateUnanswered(g) >= GATE_TRAIN_PENDING);
    if (a.res.seq == 0) return;
    MUST(gateLearn(g, s->now, g->epoch, a.res.seq, a.answer) == 1);
    MUST(!g->blind && g->missing_run == 0);
    MUST(g->cfg.kind != GATE_KIND_SCORE || g->candidate != NULL || g->serving != NULL ||
         g->generations >= TOP);
}

/* 'steps' operations of the random client of drive(), from the time 's'
 * shows; the gate must close after every few of them. */
static void use(sim *s, gate *g, uint64_t seed, int steps, const char *when) {
    driver d;

    driver_init(&d, g, seed);
    d.s.now = s->now;
    for (int i = 0; i < steps; i++) {
        driver_step(&d, g);
        if (i % 8 == 7 || i == steps - 1) closes(g, when);
    }
    s->now = d.s.now;
}

/* This is what happens to every hostile blob that decodes. If the gate is
 * blind, a request and its answer. Then the answers of its newer requests
 * arrive (newest first, so that they pile up behind the oldest), proof
 * gives up waiting for the others, their answers arrive late, some more
 * traffic, and then silence until everything has timed out and the clock
 * is at its end; and if that leaves the gate blind, a request and its
 * answer again. The gate must be consistent throughout and close at the
 * end. With 'thorough' it must close after every step, and there is more:
 * a client that does a bit of everything before the silence and after it
 * (when time has stopped), a recovery and a reset, which set the clock
 * back, and that client again after each. */
static void exercise(gate *g, uint64_t seed, int thorough) {
    uint64_t answers = g->cfg.kind == GATE_KIND_CLASSIFIER ? g->cfg.nclasses : 2;
    int64_t grace = g->cfg.feedback_timeout_ms / GATE_GRACE_DIVISOR;
    uint64_t ids[32];
    sim s = {seed, g->now_ms, 0};
    char err[160] = "";
    int n = 0;

    for (const gateObs *o = g->oldest; o && n < 32; o = o->next)
        if (!(o->flags & GATE_OBS_RESOLVED)) ids[n++] = o->seq;
    if (g->blind) see_again(&s, g);
    MUST(gateCheckInvariants(g, err, sizeof(err)));
    for (int i = n; i > n / 2; i--)
        gateLearn(g, s.now, g->epoch, ids[i - 1], (int32_t)(ids[i - 1] % answers));
    MUST(gateCheckInvariants(g, err, sizeof(err)));
    if (thorough) closes(g, "after the answers of the newer requests");
    /* The clock of a blob may leave no room to move: the client's goes on. */
    s.now = later(s.now, grace);
    gateMaintain(g, s.now);
    MUST(gateCheckInvariants(g, err, sizeof(err)));
    if (thorough) closes(g, "after proof has moved on");
    for (int i = n / 2; i > 0; i--)
        gateLearn(g, s.now, g->epoch, ids[i - 1], (int32_t)(ids[i - 1] % answers));
    MUST(gateCheckInvariants(g, err, sizeof(err)));
    if (thorough) closes(g, "after the late answers");
    if (s.now < LAST_MS - 1000000) {
        teach(&s, g, 4);
        sim_ask(&s, g);
        sim_ask(&s, g);
        if (thorough) use(&s, g, seed, 40, "while a client uses it");
    }
    /* Silence. The gate is handed the end of its clock, or a time beyond. */
    gateMaintain(g, seed % 2 ? INT64_MAX : LAST_MS);
    MUST(g->now_ms == LAST_MS);
    MUST(gateCheckInvariants(g, err, sizeof(err)));
    if (g->blind) {
        s.now = LAST_MS;
        see_again(&s, g);
        MUST(gateCheckInvariants(g, err, sizeof(err)));
    }
    closes(g, "at the end of the clock");
    if (!thorough) return;

    /* Time has stopped: nothing times out any more, but the gate works. */
    s.now = INT64_MAX;
    use(&s, g, seed + 1, 24, "with the clock at its end");
    gateRecover(g, 1700000000000ll + (int64_t)(seed % 1000), seed);
    MUST(g->now_ms == 1700000000000ll + (int64_t)(seed % 1000));
    closes(g, "after a recovery");
    s.now = g->now_ms;
    use(&s, g, seed + 2, 24, "after a recovery");
    /* A time from before the clock's range is its beginning. */
    gateReset(g, seed % 2 ? INT64_MIN : 0, seed + 3);
    MUST(g->now_ms == 0);
    closes(g, "after a reset");
    s.now = 0;
    use(&s, g, seed + 3, 16, "after a reset");
}

/* A long life instead: 'steps' operations of that client from where the
 * gate is, with nothing done to it first. */
static void live_long(gate *g, uint64_t seed, int steps) {
    sim s = {seed, g->now_ms, 0};

    use(&s, g, seed, steps, "in a long life");
}

/* ------------------------------ Hostile blobs ---------------------------- */

/* A copy of 'base' is changed in memory (the statements see it as 'g') and
 * encoded: but for the counters, which it holds to their limit, the encoder
 * writes what it is given. The result must not decode. */
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

/* A blob that must decode: into a gate that encodes back to the same
 * bytes, can be used, and closes whatever is done with it. */
static void check_accepted(const blob *b, uint64_t seed) {
    long before = closure_failures;
    gate *copy = decode(b);
    blob again;

    CHECK(copy != NULL);
    if (copy == NULL) return;
    again = encode(copy, 0);
    CHECK(blob_equal(b, &again));
    blob_free(&again);
    exercise(copy, seed, 1);
    CHECK_EQ_INT(closure_failures, before);
    gateFree(copy);
}

/* The same as HOSTILE for a change that is unusual but legitimate. */
#define ACCEPTED(base, ...) do { \
    gate *g = clone(base); \
    CHECK(g != NULL); \
    if (g != NULL) { \
        blob _blob; \
        __VA_ARGS__; \
        _blob = encode(g, 0); \
        check_accepted(&_blob, 77); \
        blob_free(&_blob); \
        gateFree(g); \
    } \
} while (0)

/* The encoding of 'base' with 'width' bytes at 'at' overwritten: refused
 * for this reason ... */
#define PATCHED(base, at, value, width, reason) do { \
    blob _blob = encode(base, 0); \
    put(_blob.p + (at), (value), (width)); \
    REJECT(_blob.p, _blob.n, reason); \
    blob_free(&_blob); \
} while (0)

/* ... or legitimate. */
#define PATCHED_OK(base, at, value, width) do { \
    blob _blob = encode(base, 0); \
    put(_blob.p + (at), (value), (width)); \
    check_accepted(&_blob, 78); \
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

/* The layout of a blob says what version 1 looks like now. It has changed
 * twice under the same version number (nothing was ever released). Before
 * the gate knew about lost answers the ledger had no blind flag and no run
 * of missing answers: five bytes less. Before the last round of changes
 * the ledger had no answer mark, a generation had no paired table, a drift
 * detector took 32 bytes instead of 40 and a request had no spot check
 * number. A blob of either layout, here the encodings of today without
 * those bytes, must not get in as something else. */
typedef struct cut {
    size_t at, n;
} cut;

/* 'b' without the bytes of the cuts, which are in ascending order. */
static blob without(const blob *b, const cut *cuts, size_t ncuts) {
    blob out = {gate_malloc(b->n), 0};
    size_t from = 0;

    for (size_t i = 0; i < ncuts; i++) {
        memcpy(out.p + out.n, b->p + from, cuts[i].at - from);
        out.n += cuts[i].at - from;
        from = cuts[i].at + cuts[i].n;
    }
    memcpy(out.p + out.n, b->p + from, b->n - from);
    out.n += b->n - from;
    return out;
}

TEST(previous_layouts) {
    gate *gates[] = {
        new_gate(GATE_KIND_SCORE, 0), score_with_requests(35, 1),
        new_gate(GATE_KIND_CLASSIFIER, 2), classifier_learning(36),
        classifier_with_everything(37, 2), golden_score_gate(), golden_classifier_gate(1),
        golden_blind_gate(),
    };

    for (size_t i = 0; i < COUNTOF(gates); i++) {
        const gate *g = gates[i];
        layout l = layout_of(g);
        blob b = encode(g, 0), old;
        cut cuts[256];
        size_t n = 0, bytes = 0;

        /* The layout of the first round. */
        cuts[0] = (cut){l.ledger + LEDGER_BLIND, 5};
        old = without(&b, cuts, 1);
        CHECK_EQ_INT(old.n, b.n - 5);
        REJECT_ANY(old.p, old.n);
        blob_free(&old);

        /* The layout of the second round. */
        cuts[n++] = (cut){l.ledger + LEDGER_ANSWER_MARK, 8};
        for (int serving = 0; serving < 2; serving++) {
            size_t base = serving ? l.serving : l.candidate;

            if ((serving ? g->serving : g->candidate) == NULL) continue;
            cuts[n++] = (cut){paired_at(g, base, 0),
                              (size_t)g->nrows * GATE_THRESHOLDS * PAIRED_BYTES};
            for (uint32_t row = 0; row < g->nrows; row++)
                cuts[n++] = (cut){drift_at(g, base, row) + 32, DRIFT_BYTES - 32};
        }
        for (uint32_t r = 0; r < g->npending && n < COUNTOF(cuts); r++)
            cuts[n++] = (cut){request_at(g, &l, r) + REQUEST_SPOT, 8};
        for (size_t c = 0; c < n; c++) bytes += cuts[c].n;
        old = without(&b, cuts, n);
        CHECK_EQ_INT(old.n, b.n - bytes);
        CHECK(bytes >= 8);
        REJECT_ANY(old.p, old.n);
        blob_free(&old);

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
    /* The snapshot step: from 1 to GATE_MAX_SNAPSHOT_EXAMPLES, a billion. */
    HOSTILE(ref, "the snapshot step is out of range", g->cfg.snapshot_examples = 0);
    HOSTILE(ref, "the snapshot step is out of range", g->cfg.snapshot_examples = 1000000001u);
    HOSTILE(ref, "the snapshot step is out of range", g->cfg.snapshot_examples = UINT32_MAX);
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

    PATCHED(ref, l.ledger + LEDGER_PAUSED, 2, 1, "invalid flag");
    /* The clock stays in its range (TIME in gate_core.h), at both ends. */
    HOSTILE(bare, "clock out of range", g->now_ms = INT64_MAX);
    HOSTILE(bare, "clock out of range", g->now_ms = LAST_MS + 1);
    HOSTILE(bare, "clock out of range", g->now_ms = -1);
    HOSTILE(bare, "clock out of range", g->now_ms = INT64_MIN);
    /* Counters stop at 2^62 (each of them: see every_counter_is_bounded).
     * The encoder does not write a larger one, so these are patched in. */
    PATCHED(ref, l.ledger + LEDGER_GENERATIONS, TOP + 1, 8, "counter out of range");
    PATCHED(ref, l.ledger + LEDGER_GENERATIONS, (uint64_t)1 << 63, 8, "counter out of range");
    PATCHED(ref, l.requests, TOP + 1, 8, "counter out of range");
    PATCHED(ref, l.requests, ~0ull, 8, "counter out of range");
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
    PATCHED(ref, l.trainer + 1, TOP + 1, 8, "counter out of range");
    PATCHED(ref, l.trainer + 1, (uint64_t)1 << 63, 8, "counter out of range");
    PATCHED(ref, l.trainer + 1, ~0ull, 8, "counter out of range");
    PATCHED(ref, l.candidate + 1 + GENERATION_BYTES, TOP + 1, 8, "counter out of range");
    PATCHED(ref, l.serving + 1 + GENERATION_BYTES, ~0ull, 8, "counter out of range");
    HOSTILE(ref, "weight out of range", g->trainer.w[100] = NAN);
    HOSTILE(ref, "weight out of range", g->trainer.w[0] = INFINITY);
    HOSTILE(ref, "weight out of range", g->trainer.w[24575] = 64.00001f);
    HOSTILE(ref, "weight out of range", g->trainer.b[1] = -65.0f);
    HOSTILE(ref, "weight out of range", g->candidate->model.w[7] = 1e30f);
    HOSTILE(ref, "weight out of range", g->serving->model.b[2] = -INFINITY);

    PATCHED(ref, l.candidate, 2, 1, "invalid flag");
    PATCHED(ref, l.serving, 0xff, 1, "invalid flag");
    PATCHED(ref, l.candidate + 1 + GENERATION_SAMPLES, TOP + 1, 8, "counter out of range");
    PATCHED(ref, l.candidate + 1 + GENERATION_SAMPLES, (uint64_t)1 << 63, 8,
            "counter out of range");
    PATCHED(ref, l.serving + 1 + GENERATION_AUDITS, TOP + 1, 8, "counter out of range");
    PATCHED(ref, l.serving + 1 + GENERATION_AUDITS, ~0ull, 8, "counter out of range");
    HOSTILE(ref, "non-finite number", g->candidate->regions[1].logw[2] = NAN);
    HOSTILE(ref, "non-finite number", g->serving->regions[40].logw[0] = INFINITY);
    HOSTILE(ref, "non-finite number", g->serving->regions[95].logw[1] = -INFINITY);
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
    /* The numbers of the generations a request names, in the blob: eight
     * bytes each, held to the snapshots the gate has taken (7). */
    PATCHED(ref, request_at(ref, &l, 2) + REQUEST_CAND_GEN, 8, 8, "invalid request guess");
    PATCHED(ref, request_at(ref, &l, 2) + REQUEST_SERV_GEN, TOP, 8, "invalid request guess");
    PATCHED(ref, request_at(ref, &l, 2) + REQUEST_SERV_GEN + 7, 0x80, 1, "invalid request guess");
    PATCHED(ref, request_at(ref, &l, 2) + REQUEST_CAND_GEN + 4, 1, 1, "invalid request guess");
    HOSTILE(score, "invalid request guess", g->oldest->serv_label = 0);
    HOSTILE(score, "invalid request guess", g->oldest->cand_label = 0);
    /* A guess of a label that is never answered proves nothing, and a
     * request cannot be proof for the candidate through one: the core
     * never makes it so, and would count such a request in the candidate's
     * paired table without counting it as a sample. (Request 9 holds such
     * a guess of the candidate, and is no proof.) */
    HOSTILE(ref, "invalid request guess", gateFindObservation(g, 5)->cand_label = 2);
    HOSTILE(ref, "invalid request guess", gateFindObservation(g, 3)->cand_label = 2);
    HOSTILE(ref, "invalid request guess", gateFindObservation(g, 6)->cand_label = 2);
    HOSTILE(ref, "invalid request guess", gateFindObservation(g, 9)->flags |= GATE_OBS_CAND);
    CHECK(gateFindObservation(ref, 9)->cand_label == 2 && ref->cfg.class_defer[2]);

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
    /* 60 clues are 480 bytes: 388 follow (its own three clues, three more
     * requests with two clues, and the counters). */
    PATCHED(ref, at, 60, 4, "truncated");

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

/* The answer mark: the number the next request had when the last answer
 * arrived. Anything from 0 (no answer yet) to the next request number. */
TEST(hostile_answer_mark) {
    gate *ref = golden_classifier_gate(1), *blind = golden_blind_gate();
    layout l = layout_of(ref), lb = layout_of(blind);
    size_t at = l.ledger + LEDGER_ANSWER_MARK;

    CHECK(ref->next_seq == 11 && ref->answer_mark == 4);
    HOSTILE(ref, "invalid request counter", g->answer_mark = 12);
    HOSTILE(ref, "invalid request counter", g->answer_mark = TOP);
    PATCHED(blind, lb.ledger + LEDGER_ANSWER_MARK, 0x12345678aull, 8, "invalid request counter");
    /* Eight bytes: with a 1 in the fifth it is 2^32 more than it was. */
    PATCHED(ref, at + 4, 1, 1, "invalid request counter");
    PATCHED(ref, at, TOP + 1, 8, "counter out of range");
    PATCHED(ref, at, ~0ull, 8, "counter out of range");
    PATCHED(ref, at + 7, 0x80, 1, "counter out of range");

    ACCEPTED(ref, g->answer_mark = 0);
    ACCEPTED(ref, g->answer_mark = 11);
    ACCEPTED(ref, g->answer_mark = 2);
    ACCEPTED(blind, g->answer_mark = g->next_seq);
    ACCEPTED(blind, g->answer_mark = 1);
    /* The last request number there is, and the last answer arrived when
     * it had been handed out. */
    ACCEPTED(ref, g->next_seq = TOP; g->answer_mark = TOP);
    gateFree(ref);
    gateFree(blind);
}

/* The paired table of a generation (nested like its regions, and no entry
 * above its samples) and the requests it missed. In the reference
 * classifier the candidate has 12 samples and its table holds 10, 10, 8, 3
 * for the first label and 7, 2 for the second; the serving generation has
 * 90 samples and a 6 in the row of the second label. */
TEST(hostile_paired_table) {
    gate *ref = golden_classifier_gate(1), *score = golden_score_gate();
    layout l = layout_of(ref), ls = layout_of(score);

    /* A higher threshold never holds more. */
    HOSTILE(ref, "candidate: paired counts inconsistent", g->candidate->paired[3] = 9);
    HOSTILE(ref, "candidate: paired counts inconsistent", g->candidate->paired[5] = 1);
    HOSTILE(ref, "candidate: paired counts inconsistent", g->candidate->paired[31] = 1);
    HOSTILE(ref, "candidate: paired counts inconsistent", g->candidate->paired[34] = 3);
    HOSTILE(ref, "candidate: paired counts inconsistent", g->candidate->paired[0] = 9);
    /* No entry is above the samples. */
    HOSTILE(ref, "candidate: paired counts inconsistent", g->candidate->paired[0] = 13);
    HOSTILE(ref, "candidate: paired counts inconsistent", g->candidate->paired[32] = 13);
    HOSTILE(ref, "candidate: paired counts inconsistent", g->candidate->samples = 9);
    /* The table of the serving generation is held to the same (the core
     * clears it when a candidate takes over). */
    HOSTILE(ref, "serving: paired counts inconsistent", g->serving->paired[33] = 7);
    HOSTILE(ref, "serving: paired counts inconsistent", g->serving->paired[0] = 91);
    HOSTILE(score, "serving: paired counts inconsistent", g->serving->paired[2] = 5);
    HOSTILE(score, "serving: paired counts inconsistent", g->serving->paired[0] = 51);
    PATCHED(ref, paired_at(ref, l.candidate, 0), TOP + 1, 8, "counter out of range");
    PATCHED(ref, paired_at(ref, l.candidate, 95), ~0ull, 8, "counter out of range");
    PATCHED(ref, paired_at(ref, l.serving, 32) + 7, 0x80, 1, "counter out of range");
    PATCHED(score, paired_at(score, ls.serving, 31), TOP + 1, 8, "counter out of range");

    /* Rows are separate: the first entry of a row may hold more than the
     * last of the row before it. A row may be full, or the table empty. */
    ACCEPTED(ref, g->candidate->paired[32] = 12);
    ACCEPTED(ref, for (uint32_t t = 0; t < GATE_THRESHOLDS; t++) g->candidate->paired[t] = 12);
    ACCEPTED(ref, memset(g->candidate->paired, 0, sizeof(uint64_t) * 3 * GATE_THRESHOLDS));
    ACCEPTED(ref, g->serving->paired[32] = 90; g->serving->paired[33] = 90);
    /* The row of a label that is never answered: the core leaves it
     * empty and never looks at it. */
    ACCEPTED(ref, g->candidate->paired[64] = 1);
    ACCEPTED(score, g->serving->paired[0] = 50);

    /* The requests a candidate missed: a counter like the others. */
    PATCHED(ref, l.candidate + 1 + GENERATION_MISSED, TOP + 1, 8, "counter out of range");
    PATCHED(ref, l.serving + 1 + GENERATION_MISSED, ~0ull, 8, "counter out of range");
    ACCEPTED(ref, g->candidate->missed = TOP; g->serving->missed = 0);
    /* Missed requests use up the budget like samples: one more and this
     * candidate is dropped (12 samples, 547 missed, a budget of 560). */
    ACCEPTED(ref, g->candidate->missed = 547);
    gateFree(ref);
    gateFree(score);
}

/* A decoded gate passes gateCheckInvariants(), and the decoder leaves to it
 * what it states. It states the rules of the new fields by itself (the
 * decoder asks two of them before it gets there, with words of its own). */
TEST(invariants_of_the_new_fields) {
    gate *g = golden_classifier_gate(1);
    char err[160] = "";

    CHECK(gateCheckInvariants(g, err, sizeof(err)));
    g->answer_mark = g->next_seq + 1;
    CHECK(!gateCheckInvariants(g, err, sizeof(err)));
    CHECK_EQ_STR(err, "answer mark beyond next_seq");
    g->answer_mark = g->next_seq;
    CHECK(gateCheckInvariants(g, err, sizeof(err)));
    g->missing_run = GATE_MISSING_RUN;
    CHECK(!gateCheckInvariants(g, err, sizeof(err)));
    CHECK_EQ_STR(err, "a run of missing answers that raised no alarm");
    g->missing_run = GATE_MISSING_RUN - 1;
    CHECK(gateCheckInvariants(g, err, sizeof(err)));
    g->candidate->paired[4] = 4; /* more than the 3 of the threshold below */
    CHECK(!gateCheckInvariants(g, err, sizeof(err)));
    CHECK_EQ_STR(err, "candidate: paired counts inconsistent");
    g->candidate->paired[4] = 3;
    CHECK(gateCheckInvariants(g, err, sizeof(err)));
    g->serving->paired[32] = 91; /* more than its 90 samples */
    CHECK(!gateCheckInvariants(g, err, sizeof(err)));
    CHECK_EQ_STR(err, "serving: paired counts inconsistent");
    gateFree(g);
}

/* What the table is for, with the table a blob holds: the candidate takes
 * over when its policy holds more of its proof requests than the table
 * says the serving generation's policy holds of them, at the threshold
 * that generation answers from now. In the reference classifier that is
 * threshold 4 of the second label (the first label has none), and the
 * candidate's policy holds 9 requests (region 1 of its first label). The
 * answer of request 5 arrives: proof for the candidate (its guess of the
 * second label was wrong), paired in regions 0 and 1 of the first label. */
TEST(paired_table_at_the_serving_policy) {
    gate *ref = golden_classifier_gate(1), *d;

    CHECK(ref->serving->policy[0] == -1 && ref->serving->policy[1] == 4);
    CHECK(ref->candidate->policy[0] == 1 && ref->candidate->regions[1].n == 9);
    /* Eight at that threshold, and more at the wider ones: 9 against 8. */
    DECODED(d, ref, for (uint32_t t = 0; t < 4; t++) g->candidate->paired[32 + t] = 12;
            g->candidate->paired[32 + 4] = 8);
    if (d != NULL) {
        CHECK_EQ_INT(gateLearn(d, d->now_ms, d->epoch, 5, 0), 1);
        CHECK(d->serving != NULL && d->serving->gen == 7 && d->candidate == NULL);
        CHECK_EQ_INT(d->stats.promotions, 3);
        CHECK(d->serving && d->serving->paired[0] == 0 && d->serving->paired[32] == 0);
        CHECK(check_copy(d));
        gateFree(d);
    }
    /* Nine at that threshold: 9 against 9, and after the answer of request
     * 6, which was waiting behind, 10 against 10. It does not take over. */
    DECODED(d, ref, for (uint32_t t = 0; t < 4; t++) g->candidate->paired[32 + t] = 12;
            g->candidate->paired[32 + 4] = 9);
    if (d != NULL) {
        CHECK_EQ_INT(gateLearn(d, d->now_ms, d->epoch, 5, 0), 1);
        CHECK(d->serving != NULL && d->serving->gen == 5);
        CHECK(d->candidate != NULL && d->candidate->gen == 7 && d->candidate->samples == 14);
        CHECK(d->candidate && d->candidate->regions[1].n == 10);
        /* Request 5 was paired in two regions of the first label (the
         * serving generation scored 0.5), request 6 in twelve of the
         * second (0.9375: threshold 11 is 0.9336, threshold 12 0.9462). */
        CHECK(d->candidate && d->candidate->paired[0] == 11 && d->candidate->paired[2] == 8);
        CHECK(d->candidate && d->candidate->paired[32 + 4] == 10);
        CHECK(d->candidate && d->candidate->paired[32 + 11] == 1 && d->candidate->paired[32 + 12] == 0);
        CHECK_EQ_INT(d->stats.promotions, 2);
        CHECK(check_copy(d));
        gateFree(d);
    }
    gateFree(ref);
}

/* The spot checks of the reference score gate get these numbers, so that
 * its detector may hold fewer than the 70 they were handed out of. */
static void set_asked(gate *g, uint64_t asked) {
    g->serving->drift[0].asked = asked;
    g->oldest->spot = 1;
    g->newest->spot = asked;
}

/* The drift detectors. Disagreements and lost answers are counted for the
 * block now open of each length, and there are no more of them together
 * than spot checks asked in that block; and no more disagreements in all
 * than spot checks. Of the 600 spot checks of the reference classifier's
 * second label, 24 are in the open block of 64, 88 in that of 512 and all
 * in that of 4,096; it holds 1, 2, 3 disagreements and 4, 5, 6 lost
 * answers. */
TEST(hostile_drift_detectors) {
    gate *ref = golden_classifier_gate(1), *score = golden_score_gate();
    layout l = layout_of(ref);
    size_t at = drift_at(ref, l.serving, 1);
    gate *g;
    blob b;

    HOSTILE(ref, "invalid drift detector", g->serving->drift[1].wrong[0] = 21);
    ACCEPTED(ref, g->serving->drift[1].wrong[0] = 20);
    HOSTILE(ref, "invalid drift detector", g->serving->drift[1].lost[0] = 24);
    ACCEPTED(ref, g->serving->drift[1].lost[0] = 23);
    HOSTILE(ref, "invalid drift detector", g->serving->drift[1].wrong[1] = 84);
    ACCEPTED(ref, g->serving->drift[1].wrong[1] = 83);
    HOSTILE(ref, "invalid drift detector", g->serving->drift[1].lost[1] = 87);
    ACCEPTED(ref, g->serving->drift[1].lost[1] = 86);
    HOSTILE(ref, "invalid drift detector", g->serving->drift[1].wrong[2] = 595);
    ACCEPTED(ref, g->serving->drift[1].wrong[2] = 594);
    HOSTILE(ref, "invalid drift detector", g->serving->drift[1].lost[2] = 598);
    ACCEPTED(ref, g->serving->drift[1].lost[2] = 597);
    /* The two are added without wrapping around. */
    HOSTILE(ref, "invalid drift detector",
            g->serving->drift[1].wrong[0] = g->serving->drift[1].lost[0] = 0x80000000u);
    HOSTILE(ref, "invalid drift detector",
            g->serving->drift[1].wrong[2] = 0xffffffffu; g->serving->drift[1].lost[2] = 1);
    /* 32 bits each, the disagreements of the three lengths first: the top
     * byte of the first count, and of the last. */
    PATCHED(ref, at + DRIFT_WRONG + 3, 1, 1, "invalid drift detector");
    PATCHED(ref, at + DRIFT_LOST + 11, 0x80, 1, "invalid drift detector");
    /* A detector that has asked nothing has no block open: the one of the
     * label that is never answered, and those of a candidate. */
    HOSTILE(ref, "invalid drift detector", g->serving->drift[2].wrong[2] = 1);
    HOSTILE(ref, "invalid drift detector", g->serving->drift[2].lost[0] = 1);
    HOSTILE(ref, "invalid drift detector", g->candidate->drift[0].lost[1] = 1);

    /* Where the blocks begin and end. With 64 asked the first block of 64
     * is the open one, and full; with 65 the second, with one spot check
     * in it. The same at 512 and 513, and at 4,096 and 4,097. */
    ACCEPTED(score, set_asked(g, 64); g->serving->drift[0].wrong[0] = 40;
             g->serving->drift[0].lost[0] = 24);
    HOSTILE(score, "invalid drift detector", set_asked(g, 64);
            g->serving->drift[0].wrong[0] = 41; g->serving->drift[0].lost[0] = 24);
    ACCEPTED(score, set_asked(g, 65); g->serving->drift[0].wrong[0] = 0;
             g->serving->drift[0].lost[0] = 1; g->serving->drift[0].wrong[1] = 40;
             g->serving->drift[0].lost[1] = 25);
    HOSTILE(score, "invalid drift detector", set_asked(g, 65);
            g->serving->drift[0].wrong[0] = 1; g->serving->drift[0].lost[0] = 1);
    ACCEPTED(score, set_asked(g, 512); g->serving->drift[0].wrong[1] = 500;
             g->serving->drift[0].lost[1] = 12; g->serving->drift[0].lost[0] = 63;
             g->serving->drift[0].errors = 512);
    HOSTILE(score, "invalid drift detector", set_asked(g, 512);
            g->serving->drift[0].wrong[1] = 500; g->serving->drift[0].lost[1] = 13);
    ACCEPTED(score, set_asked(g, 513); g->serving->drift[0].lost[0] = 0;
             g->serving->drift[0].wrong[1] = 1; g->serving->drift[0].lost[1] = 0;
             g->serving->drift[0].wrong[2] = 510);
    HOSTILE(score, "invalid drift detector", set_asked(g, 513);
            g->serving->drift[0].lost[0] = 0; g->serving->drift[0].wrong[1] = 1;
            g->serving->drift[0].lost[1] = 1);
    ACCEPTED(score, set_asked(g, 4096); g->serving->drift[0].wrong[2] = 96;
             g->serving->drift[0].lost[2] = 4000);
    HOSTILE(score, "invalid drift detector", set_asked(g, 4096);
            g->serving->drift[0].wrong[2] = 97; g->serving->drift[0].lost[2] = 4000);
    ACCEPTED(score, set_asked(g, 4097); g->serving->drift[0].wrong[2] = 1;
             g->serving->drift[0].lost[2] = 0; g->serving->drift[0].wrong[0] = 1;
             g->serving->drift[0].lost[0] = 0; g->serving->drift[0].wrong[1] = 0;
             g->serving->drift[0].lost[1] = 1);
    HOSTILE(score, "invalid drift detector", set_asked(g, 4097);
            g->serving->drift[0].lost[0] = 0; g->serving->drift[0].wrong[1] = 1;
            g->serving->drift[0].lost[1] = 0; g->serving->drift[0].wrong[2] = 1;
            g->serving->drift[0].lost[2] = 1);
    /* The last spot check there is: 2^62 is a multiple of all three
     * lengths, so every block is full. */
    ACCEPTED(score, set_asked(g, TOP); g->serving->drift[0].errors = TOP;
             g->serving->drift[0].wrong[0] = 64; g->serving->drift[0].lost[0] = 0;
             g->serving->drift[0].wrong[1] = 0; g->serving->drift[0].lost[1] = 512;
             g->serving->drift[0].wrong[2] = 4000; g->serving->drift[0].lost[2] = 96);
    HOSTILE(score, "invalid drift detector", set_asked(g, TOP);
            g->serving->drift[0].wrong[0] = 65; g->serving->drift[0].lost[0] = 0);

    /* No more disagreements than spot checks. The encoder never writes
     * more (see below), so these are patched in. */
    PATCHED(ref, at + DRIFT_ERRORS, 601, 8, "invalid drift detector");
    PATCHED_OK(ref, at + DRIFT_ERRORS, 600, 8);
    PATCHED_OK(ref, at + DRIFT_ERRORS, 0, 8);
    PATCHED(ref, drift_at(ref, l.serving, 2) + DRIFT_ERRORS, 1, 8, "invalid drift detector");
    PATCHED(ref, drift_at(ref, l.candidate, 0) + DRIFT_ERRORS, 1, 8, "invalid drift detector");
    PATCHED(ref, at + DRIFT_ASKED, TOP + 1, 8, "counter out of range");
    PATCHED(ref, at + DRIFT_ASKED + 7, 0x80, 1, "counter out of range");
    PATCHED(ref, at + DRIFT_ERRORS, TOP + 1, 8, "counter out of range");
    PATCHED(ref, at + DRIFT_ERRORS, ~0ull, 8, "counter out of range");

    /* What the encoder makes of a detector that holds more disagreements
     * than spot checks (the core counts one for each disagreeing answer it
     * is told of, whether or not the detector handed out that many): as
     * many as spot checks. And at most the limit of every counter. */
    g = clone(ref);
    g->serving->drift[1].errors = 601;
    b = encode(g, 0);
    CHECK_EQ_INT(get(b.p + at + DRIFT_ERRORS, 8), 600);
    CHECK_EQ_INT(get(b.p + at + DRIFT_ASKED, 8), 600);
    check_accepted(&b, 79);
    blob_free(&b);
    g->serving->drift[1].errors = ~0ull;
    g->serving->drift[2].errors = 3;
    b = encode(g, 0);
    CHECK_EQ_INT(get(b.p + at + DRIFT_ERRORS, 8), 600);
    CHECK_EQ_INT(get(b.p + drift_at(ref, l.serving, 2) + DRIFT_ERRORS, 8), 0);
    check_accepted(&b, 80);
    blob_free(&b);
    g->serving->drift[1].asked = TOP + 64 * 9 + 24; /* the same place in every block */
    g->serving->drift[1].errors = TOP + 1000;
    gateFindObservation(g, 6)->spot = TOP + 599;
    b = encode(g, 0);
    CHECK(get(b.p + at + DRIFT_ERRORS, 8) == TOP && get(b.p + at + DRIFT_ASKED, 8) == TOP);
    check_accepted(&b, 81);
    blob_free(&b);
    gateFree(g);

    gateFree(ref);
    gateFree(score);
}

/* Spot checks: requests that carry a number of the drift detector. */
TEST(hostile_spot_checks) {
    gate *ref = golden_classifier_gate(1), *score = golden_score_gate();
    gate *blind = golden_blind_gate();
    layout l = layout_of(ref), ls = layout_of(score);
    const uint32_t spot_flags = GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT;

    /* A spot check is an audited request that the serving generation
     * would have answered: it carries both flags, whatever has become of
     * its proof flag, and the number of that generation. */
    HOSTILE(score, "invalid spot check", g->oldest->next->spot = 1);
    HOSTILE(ref, "invalid spot check", gateFindObservation(g, 9)->spot = 1);
    HOSTILE(ref, "invalid spot check", gateFindObservation(g, 2)->spot = 1);
    HOSTILE(ref, "invalid spot check", gateFindObservation(g, 5)->spot = 1);
    HOSTILE(ref, "invalid spot check", gateFindObservation(g, 3)->spot = 600);
    HOSTILE(ref, "invalid spot check", gateFindObservation(g, 10)->flags = GATE_OBS_AUDIT);
    HOSTILE(ref, "invalid spot check", gateFindObservation(g, 10)->flags = GATE_OBS_WOULD_ACCEPT);
    HOSTILE(ref, "invalid spot check", gateFindObservation(g, 6)->flags &= ~GATE_OBS_AUDIT);
    HOSTILE(blind, "invalid spot check", g->oldest->next->flags = spot_flags;
            g->oldest->next->spot = 1);
    HOSTILE(ref, "invalid spot check", gateFindObservation(g, 10)->serv_gen = 0;
            gateFindObservation(g, 10)->serv_label = -1);

    /* Its number is one the detector of its row has handed out, if the
     * generation still serves: 600 for the second label of the reference
     * classifier, 2 for the first, none for the one that is never
     * answered; 70 in the reference score gate. */
    HOSTILE(ref, "invalid spot check", gateFindObservation(g, 6)->spot = 601);
    ACCEPTED(ref, gateFindObservation(g, 6)->spot = 600);
    ACCEPTED(ref, gateFindObservation(g, 6)->spot = 1);
    HOSTILE(ref, "invalid spot check", gateFindObservation(g, 6)->serv_label = 0);
    ACCEPTED(ref, gateFindObservation(g, 6)->serv_label = 0; gateFindObservation(g, 6)->spot = 2);
    HOSTILE(ref, "invalid spot check", gateFindObservation(g, 6)->serv_label = 0;
            gateFindObservation(g, 6)->spot = 3);
    HOSTILE(ref, "invalid spot check", gateFindObservation(g, 6)->serv_label = 2;
            gateFindObservation(g, 6)->spot = 1);
    HOSTILE(score, "invalid spot check", g->newest->spot = 71);
    HOSTILE(score, "invalid spot check", g->oldest->spot = TOP);
    ACCEPTED(score, g->oldest->spot = 70); /* the same number twice */
    /* Also when its proof flag is gone (the request is kept for training). */
    HOSTILE(ref, "invalid spot check", gateFindObservation(g, 6)->flags = spot_flags;
            gateFindObservation(g, 6)->result = -1; gateFindObservation(g, 6)->spot = 601);
    ACCEPTED(ref, gateFindObservation(g, 6)->flags = spot_flags;
             gateFindObservation(g, 6)->result = -1);
    PATCHED(ref, request_at(ref, &l, 5) + REQUEST_SPOT, TOP + 1, 8, "counter out of range");
    PATCHED(ref, request_at(ref, &l, 3) + REQUEST_SPOT, ~0ull, 8, "counter out of range");
    PATCHED(score, request_at(score, &ls, 0) + REQUEST_SPOT + 7, 0x80, 1, "counter out of range");

    /* The spot check of a generation that is gone keeps whatever number
     * it had: the detector that handed it out went with the generation. */
    ACCEPTED(ref, gateFindObservation(g, 10)->spot = TOP);
    ACCEPTED(ref, gateFindObservation(g, 10)->spot = 1);
    ACCEPTED(ref, gateFindObservation(g, 6)->serv_gen = 4);
    ACCEPTED(score, g->oldest->serv_gen = 2; g->newest->serv_gen = 1; g->oldest->spot = TOP);
    /* A request that would be a spot check and has no number is none to
     * the detector. */
    ACCEPTED(ref, gateFindObservation(g, 6)->spot = 0);
    ACCEPTED(score, g->oldest->spot = 0; g->newest->spot = 0);

    /* No request was a spot check of the candidate, which has not served
     * yet. But it may come to serve, with the detectors the blob gives
     * it, and then the number must be one of theirs: so it must be now. */
    HOSTILE(ref, "invalid spot check", gateFindObservation(g, 10)->serv_gen = 7);
    HOSTILE(ref, "invalid spot check", gateFindObservation(g, 10)->serv_gen = 7;
            g->candidate->drift[1].asked = 0x0102030404ull);
    ACCEPTED(ref, gateFindObservation(g, 10)->serv_gen = 7;
             g->candidate->drift[1].asked = 0x0102030405ull);

    gateFree(ref);
    gateFree(score);
    gateFree(blind);
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
    long before = closure_failures;

    /* The request waits alone, behind an unanswered piece of evidence, or
     * for generations that are gone; and it carries the number of a spot
     * check (the seventh of the 600 of its label) or none. */
    for (int variant = 0; variant < 6; variant++) {
        int behind = variant % 3 == 1, gone = variant % 3 == 2, spot = variant >= 3;

        for (uint32_t flags = 0; flags < 64; flags++) {
            int evidence = (flags & GATE_OBS_EVIDENCE) != 0;
            int answered = (flags & GATE_OBS_RESOLVED) != 0;
            int provisional = (flags & GATE_OBS_PROVISIONAL) != 0;
            int queued = evidence && !provisional;
            /* Which of the three rules the request breaks. */
            int broken[3] = {answered && !evidence, provisional && (!evidence || answered),
                             answered && queued && !behind && !gone};
            /* A spot check was audited and would have been answered. */
            int no_spot_check = spot && (flags & 3) != 3;
            int valid = !broken[0] && !broken[1] && !broken[2] && !no_spot_check;
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
            if (spot) o->spot = 7;
            gateObsLink(g, o);
            b = encode(g, 0);
            copy = gateDecode(b.p, b.n, err, sizeof(err));
            MUST((copy != NULL) == valid);
            if (copy != NULL) {
                const gateObs *last = copy->newest;

                accepted++;
                /* The decoder rebuilt what is not stored. */
                MUST(last->flags == flags);
                MUST(last->spot == (spot ? 7u : 0u));
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
                /* The spot check is looked at first, with the request. */
                if (no_spot_check) {
                    known = strcmp(err, "invalid spot check") == 0;
                } else {
                    for (int i = 0; i < 3; i++)
                        if (broken[i] && strcmp(err, reasons[i]) == 0) known = 1;
                }
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
     * fine too: 3 more, times four: 40 each. With a spot check number
     * bits 1 and 2 are both set, one way instead of four: 7 + 10 + 10. */
    CHECK_EQ_INT(accepted, 28 + 40 + 40 + 7 + 10 + 10);
    CHECK_EQ_INT(refused, 6 * 64 - 135);
    CHECK_EQ_INT(closure_failures, before);

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
    size_t at = layout_of(ref).counters;

    HOSTILE(ref, "request counters do not add up", g->stats.requests++);
    HOSTILE(ref, "request counters do not add up", g->stats.abstained--);
    HOSTILE(ref, "request counters do not add up", g->stats.requests = TOP);
    HOSTILE(ref, "request counters do not add up", g->stats.accepted = TOP);
    /* What GINFO shows is shown as a signed number: every counter stops
     * at 2^62, far short of the range in which it would be negative. Each
     * of the four request counters on its own, patched in (the encoder
     * holds them to the limit): the "negative" ones would add up, modulo
     * 2^64. */
    PATCHED(ref, at + COUNTERS_REQUESTS, TOP + 1, 8, "counter out of range");
    PATCHED(ref, at + COUNTERS_REQUESTS, ~0ull, 8, "counter out of range");
    PATCHED(ref, at + COUNTERS_ACCEPTED, (uint64_t)-100, 8, "counter out of range");
    PATCHED(ref, at + COUNTERS_AUDITED, (uint64_t)-100, 8, "counter out of range");
    PATCHED(ref, at + COUNTERS_ABSTAINED, (uint64_t)-100, 8, "counter out of range");
    PATCHED(ref, at + COUNTERS_ABSTAINED, TOP + 1, 8, "counter out of range");
    /* The others: ids handed out, feedback used and not, answers missing,
     * requests dropped, promotions, revocations, snapshots retired. */
    for (size_t field = COUNTERS_OBSERVATIONS; field < COUNTERS_BYTES; field += 8) {
        PATCHED(ref, at + field, TOP + 1, 8, "counter out of range");
        PATCHED(ref, at + field + 7, 0x80, 1, "counter out of range");
    }
    HOSTILE(ref, "coverage out of range", g->stats.window = GATE_COVERAGE_WINDOW + 1);
    PATCHED(ref, at + COUNTERS_WINDOW, TOP + 1, 8, "counter out of range");
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

    /* Both ends of the clock's range. */
    ACCEPTED(ref, g->now_ms = LAST_MS);
    ACCEPTED(ref, set_all_times(g, LAST_MS));
    ACCEPTED(ref, set_all_times(g, 0));
    ACCEPTED(ref, g->oldest->created_ms = 0); /* as old as a request can be */
    /* Times nothing is computed from may be anything. */
    ACCEPTED(ref, g->created_ms = INT64_MIN; g->candidate->frozen_ms = -1;
             g->serving->frozen_ms = INT64_MAX; g->serving->promoted_ms = INT64_MIN);
    ACCEPTED(ref, g->lineage = g->epoch = ~0ull);
    /* The last snapshot number and the last request number: a gate that
     * has used them up still holds the requests that wait. */
    ACCEPTED(ref, g->generations = TOP; g->next_seq = TOP);
    ACCEPTED(ref, g->generations = TOP; g->candidate->gen = TOP; g->serving->gen = TOP - 1);
    ACCEPTED(score, g->next_seq = TOP; g->newest->seq = TOP - 1; g->answer_mark = TOP - 1);
    /* Every counter may be at its limit (each on its own, and all of a
     * blob at once: see the tests of the limits below). */
    ACCEPTED(ref, g->candidate->samples = g->serving->audits = TOP);
    ACCEPTED(ref, g->trainer.updates = TOP; g->snapshot_updates = 5);
    ACCEPTED(ref, g->trainer.updates = 0; g->snapshot_updates = TOP);
    ACCEPTED(ref, g->candidate->model.updates = g->serving->model.updates = TOP);
    ACCEPTED(ref, g->stats.requests = TOP; g->stats.accepted = TOP - 300);
    ACCEPTED(ref, g->stats.requests = TOP; g->stats.accepted = 0; g->stats.audited = TOP;
             g->stats.abstained = 0);
    ACCEPTED(ref, g->stats.requests = TOP; g->stats.accepted = g->stats.audited = 0;
             g->stats.abstained = TOP);
    ACCEPTED(ref, g->stats.missing = g->stats.revocations = TOP);
    ACCEPTED(ref, g->candidate->budget = TOP; g->candidate->missed = TOP);
    ACCEPTED(ref, g->candidate->budget = 0; g->serving->budget = TOP);
    ACCEPTED(ref, g->stats.full = g->stats.observations = g->stats.learned = TOP);
    ACCEPTED(ref, g->stats.ignored = g->stats.dropped = g->stats.promotions =
             g->stats.retired = TOP);
    ACCEPTED(ref, g->stats.window = GATE_COVERAGE_WINDOW; g->stats.coverage = 1.0);
    ACCEPTED(ref, g->stats.window = 0; g->stats.coverage = -0.0);

    /* The largest snapshot step is a billion (GATE_MAX_SNAPSHOT_EXAMPLES). */
    ACCEPTED(ref, g->cfg.audit = 1.0; g->cfg.delta = 0.999;
             g->cfg.snapshot_examples = 1000000000u);
    ACCEPTED(ref, g->cfg.feedback_timeout_ms = GATE_MAX_FEEDBACK_TIMEOUT_MS);
    ACCEPTED(ref, g->cfg.feedback_timeout_ms = 1);
    ACCEPTED(ref, g->cfg.alpha = 1e-300; g->cfg.class_alpha[0] = g->cfg.class_alpha[1] =
             g->cfg.class_alpha[2] = 5e-324);
    ACCEPTED(ref, g->trainer.w[0] = -64.0f; g->trainer.w[1] = -0.0f; g->trainer.b[0] = FLT_MIN);
    ACCEPTED(ref, g->candidate->regions[0].logw[0] = -DBL_MAX;
             g->serving->regions[40].logw[1] = DBL_MAX);
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
     * whose snapshot could not be allocated starts like that, and one that
     * has used up its snapshot numbers ends like that. */
    ACCEPTED(blind, g->blind = 0);
    ACCEPTED(blind, g->blind = 0; g->generations = TOP);

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
             gateFindObservation(g, 6)->result = -1; gateFindObservation(g, 6)->spot = 0);
    /* ... every waiting request passed by, or none ... */
    ACCEPTED(score, g->oldest->next->flags |= GATE_OBS_PROVISIONAL;
             g->newest->flags = GATE_OBS_SERV | GATE_OBS_PROVISIONAL; g->newest->result = -1;
             g->newest->spot = 0);
    ACCEPTED(ref, gateFindObservation(g, 3)->flags &= ~(uint32_t)GATE_OBS_PROVISIONAL);
    /* ... proof flags without the generation they name, a serving guess
     * of a label that is never answered that claims to be proof, and a
     * request that is proof for one generation through both its guesses. */
    ACCEPTED(score, g->oldest->flags |= GATE_OBS_CAND);
    ACCEPTED(ref, gateFindObservation(g, 3)->flags |= GATE_OBS_SERV);
    ACCEPTED(ref, gateFindObservation(g, 5)->serv_label = 2);
    ACCEPTED(ref, gateFindObservation(g, 5)->cand_gen = 5);
    /* A candidate that has a proven threshold while nothing serves. */
    ACCEPTED(score, g->candidate = g->serving; g->serving = NULL; g->candidate->promoted_ms = 0;
             g->candidate->audits = g->candidate->audit_errors = 0);
    /* A candidate that is older than the generation that serves, with
     * detectors that have asked spot checks, and a serving generation
     * with a paired table and missed requests. */
    ACCEPTED(ref, g->candidate->gen = 3; g->candidate->drift[0] = g->serving->drift[1]);

    gateFree(ref);
    gateFree(score);
    gateFree(blind);
}

/* --------------------------- Counters and limits --------------------------
 * CHANGES.md 9d and COUNTERS in gate_codec.c: no counter is written above
 * 2^62 and none is accepted above it, for every counter of a blob; the
 * core hands out no request number and no snapshot number beyond 2^62 and
 * holds the clock to its range. A gate at any of these limits goes through
 * the codec like every other. */

/* Every counter of a blob, one at a time: one more than the limit, the
 * sign bit, and all bits set are refused as that, whatever else the blob
 * holds. */
TEST(every_counter_is_bounded) {
    static size_t at[MAX_COUNTERS];
    static const uint64_t beyond[] = {TOP + 1, (uint64_t)1 << 63, ~0ull};
    gate *gates[] = {golden_classifier_gate(1), golden_score_gate(), golden_blind_gate()};
    /* Three in the ledger and the examples of the trainer; for each
     * generation seven, the examples of its model, four for each region
     * (three and its entry of the paired table) and two for each row; the
     * next request number and the spot check number of each request;
     * fourteen at the end. */
    static const size_t expect[] = {
        3 + 1 + 2 * (7 + 1 + 4 * 96 + 2 * 3) + 1 + 6 + 14,
        3 + (7 + 4 * 32 + 2) + 1 + 3 + 14,
        3 + 1 + 3 + 14,
    };

    for (size_t i = 0; i < COUNTOF(gates); i++) {
        layout l = layout_of(gates[i]);
        blob b = encode(gates[i], 0);
        size_t n = counter_offsets(gates[i], &l, at), bad = 0;
        gate *g;

        CHECK_EQ_INT(n, expect[i]);
        for (size_t c = 0; c < n; c++) {
            unsigned char old[8];

            memcpy(old, b.p + at[c], 8);
            for (size_t v = 0; v < COUNTOF(beyond); v++) {
                char err[160] = "";

                put(b.p + at[c], beyond[v], 8);
                g = gateDecode(b.p, b.n, err, sizeof(err));
                if ((g != NULL || strcmp(err, "counter out of range") != 0) && bad++ < 5)
                    printf("  the counter at byte %zu, set to %llx: %s\n", at[c],
                           (unsigned long long)beyond[v], g ? "decodes" : err);
                gateFree(g);
            }
            memcpy(b.p + at[c], old, 8);
        }
        CHECK_EQ_INT(bad, 0);
        g = decode(&b); /* the blob is whole again */
        CHECK(g != NULL);
        gateFree(g);
        blob_free(&b);
        gateFree(gates[i]);
    }
}

/* The counters where a gate holds them, in the order of counter_offsets()
 * (the order of the layout comment, not of the structures). 'special'
 * receives 1 for the four request counters, which are written together,
 * and 2 for the disagreements of a drift detector, which are written
 * together with its spot checks (the field before). */
static size_t counter_fields(gate *g, uint64_t **field, int *special) {
    size_t n = 0, first;

    field[n++] = &g->generations;
    field[n++] = &g->snapshot_updates;
    field[n++] = &g->answer_mark;
    if (g->cfg.kind == GATE_KIND_CLASSIFIER) field[n++] = &g->trainer.updates;
    for (int serving = 0; serving < 2; serving++) {
        gateGeneration *gen = serving ? g->serving : g->candidate;

        if (gen == NULL) continue;
        field[n++] = &gen->gen;
        field[n++] = &gen->samples;
        field[n++] = &gen->errors;
        field[n++] = &gen->missed;
        field[n++] = &gen->audits;
        field[n++] = &gen->audit_errors;
        field[n++] = &gen->budget;
        if (g->cfg.kind == GATE_KIND_CLASSIFIER) field[n++] = &gen->model.updates;
        for (uint32_t i = 0; i < g->nrows * GATE_THRESHOLDS; i++) {
            field[n++] = &gen->regions[i].n;
            field[n++] = &gen->regions[i].errors;
            field[n++] = &gen->regions[i].certified_at;
        }
        for (uint32_t i = 0; i < g->nrows * GATE_THRESHOLDS; i++) field[n++] = &gen->paired[i];
        for (uint32_t i = 0; i < g->nrows; i++) {
            field[n++] = &gen->drift[i].asked;
            special[n] = 2;
            field[n++] = &gen->drift[i].errors;
        }
    }
    field[n++] = &g->next_seq;
    for (gateObs *o = g->oldest; o; o = o->next) field[n++] = &o->spot;
    first = n;
    field[n++] = &g->stats.requests;
    field[n++] = &g->stats.accepted;
    field[n++] = &g->stats.audited;
    field[n++] = &g->stats.abstained;
    for (size_t c = first; c < n; c++) special[c] = 1;
    field[n++] = &g->stats.full;
    field[n++] = &g->stats.window;
    field[n++] = &g->stats.observations;
    field[n++] = &g->stats.learned;
    field[n++] = &g->stats.ignored;
    field[n++] = &g->stats.missing;
    field[n++] = &g->stats.dropped;
    field[n++] = &g->stats.promotions;
    field[n++] = &g->stats.revocations;
    field[n++] = &g->stats.retired;
    return n;
}

/* 1 if 'b' is 'base' with the eight bytes at 'at' holding 'value' (and, if
 * 'also' is set, the eight bytes before them holding it too) and nothing
 * else changed. */
static int only_changed(const blob *b, const blob *base, size_t at, uint64_t value, int also) {
    size_t from = also ? at - 8 : at;

    if (b->n != base->n || get(b->p + at, 8) != value) return 0;
    if (also && get(b->p + from, 8) != value) return 0;
    return memcmp(b->p, base->p, from) == 0 &&
           memcmp(b->p + at + 8, base->p + at + 8, b->n - at - 8) == 0;
}

/* The encoder, for every counter a gate holds, one at a time. A value of
 * its own arrives where the layout says and nowhere else: this places
 * every counter without the help of the decoder. The limit is written as
 * it is, and anything beyond it as the limit, also in a digest. */
TEST(every_counter_is_written_within_its_limit) {
    static size_t at[MAX_COUNTERS];
    static uint64_t *field[MAX_COUNTERS];
    static int special[MAX_COUNTERS];
    static const uint64_t beyond[] = {TOP + 1, TOP + 0x1122334455ull, (uint64_t)1 << 63, ~0ull};
    const uint64_t own = 0x1122334455667788ull;
    gate *gates[] = {golden_classifier_gate(1), golden_score_gate(), golden_blind_gate()};

    for (size_t i = 0; i < COUNTOF(gates); i++) {
        gate *g = gates[i];
        layout l = layout_of(g);
        blob base = encode(g, 0), digest = encode(g, 1), b;
        size_t n, bad = 0;

        memset(special, 0, sizeof(special));
        n = counter_offsets(g, &l, at);
        CHECK_EQ_INT(counter_fields(g, field, special), n);
        for (size_t c = 0; c < n; c++) {
            uint64_t old = *field[c], asked = 0;
            int pair = special[c] == 2, ok = 1;

            if (special[c] == 1) continue; /* see request_counters_beyond_the_limit */
            /* A drift detector is written with no more disagreements than
             * spot checks: for this, as many spot checks. */
            if (pair) asked = *field[c - 1];
            *field[c] = own;
            if (pair) *field[c - 1] = own;
            b = encode(g, 0);
            ok = ok && only_changed(&b, &base, at[c], own, pair);
            blob_free(&b);
            *field[c] = TOP;
            if (pair) *field[c - 1] = TOP;
            b = encode(g, 0);
            ok = ok && only_changed(&b, &base, at[c], TOP, pair);
            blob_free(&b);
            for (size_t v = 0; v < COUNTOF(beyond); v++) {
                *field[c] = beyond[v];
                if (pair) *field[c - 1] = beyond[v];
                b = encode(g, 0);
                ok = ok && only_changed(&b, &base, at[c], TOP, pair);
                blob_free(&b);
                /* A digest leaves out what only the primary counts (the
                 * first seven fields at the end). */
                b = encode(g, 1);
                ok = ok && only_changed(&b, &digest, at[c],
                                        at[c] >= l.counters + COUNTERS_REQUESTS &&
                                        at[c] < l.counters + COUNTERS_COVERAGE ? 0 : TOP, pair);
                blob_free(&b);
            }
            *field[c] = old;
            if (pair) *field[c - 1] = asked;
            if (!ok && bad++ < 5) printf("  the counter at byte %zu is not written as it must\n", at[c]);
        }
        CHECK_EQ_INT(bad, 0);
        b = encode(g, 0);
        CHECK(blob_equal(&b, &base));
        blob_free(&b);
        blob_free(&base);
        blob_free(&digest);
        gateFree(g);
    }
}

/* The four request counters add up, also when they are beyond the limit
 * and written as the limit: the three kinds of reply are cut down in turn
 * (accepted, audited, abstained) to what the ones before leave of the
 * limit, and the requests are their sum. Below the limit, and at it, they
 * are written as they are. */
TEST(request_counters_beyond_the_limit) {
    static const struct {
        uint64_t in[3], out[3];
    } cases[] = {
        {{5, 6, 7}, {5, 6, 7}},
        {{TOP - 2, 1, 1}, {TOP - 2, 1, 1}},
        {{0, 0, TOP}, {0, 0, TOP}},
        {{TOP + 10, 0, 0}, {TOP, 0, 0}},
        {{TOP - 5, 15, 0}, {TOP - 5, 5, 0}},
        {{TOP - 5, 3, 12}, {TOP - 5, 3, 2}},
        {{0, TOP + 1, 5}, {0, TOP, 0}},
        {{7, 8, TOP}, {7, 8, TOP - 15}},
        {{1, TOP, TOP + 9}, {1, TOP - 1, 0}},
        {{TOP, TOP, TOP}, {TOP, 0, 0}},
        {{~0ull / 4, ~0ull / 4, ~0ull / 4}, {~0ull / 4, 1, 0}},
    };
    gate *g = golden_score_gate();
    size_t at = layout_of(g).counters;

    for (size_t i = 0; i < COUNTOF(cases); i++) {
        uint64_t sum = cases[i].out[0] + cases[i].out[1] + cases[i].out[2];
        blob b;

        g->stats.accepted = cases[i].in[0];
        g->stats.audited = cases[i].in[1];
        g->stats.abstained = cases[i].in[2];
        g->stats.requests = cases[i].in[0] + cases[i].in[1] + cases[i].in[2];
        b = encode(g, 0);
        CHECK(get(b.p + at + COUNTERS_ACCEPTED, 8) == cases[i].out[0]);
        CHECK(get(b.p + at + COUNTERS_AUDITED, 8) == cases[i].out[1]);
        CHECK(get(b.p + at + COUNTERS_ABSTAINED, 8) == cases[i].out[2]);
        CHECK(get(b.p + at + COUNTERS_REQUESTS, 8) == sum);
        CHECK(sum <= TOP && sum == (g->stats.requests > TOP ? TOP : g->stats.requests));
        check_accepted(&b, 82 + i);
        blob_free(&b);
        b = encode(g, 1);
        CHECK(get(b.p + at + COUNTERS_REQUESTS, 8) == 0 && get(b.p + at + COUNTERS_ABSTAINED, 8) == 0);
        CHECK(get(b.p + at + COUNTERS_ACCEPTED, 8) == 0 && get(b.p + at + COUNTERS_AUDITED, 8) == 0);
        blob_free(&b);
    }
    gateFree(g);
}

/* The last request numbers. A gate hands out numbers up to 2^62 - 1; then
 * it is full, for good, and still holds what it recorded. */
TEST(request_numbers_run_out) {
    gate *g = new_gate(GATE_KIND_SCORE, 0);
    int64_t t0 = g->created_ms;

    g->next_seq = TOP - 3;
    g = through_codec(g);
    if (g == NULL) return;
    for (uint64_t i = 0; i < 3; i++) {
        CHECK(!gateFull(g));
        CHECK(ask_score(g, t0 + 1, 0.9, 0.9) == TOP - 3 + i);
        CHECK(check_copy(g));
    }
    CHECK(g->next_seq == TOP && gateFull(g));
    CHECK_EQ_INT(g->npending, 3);
    /* Full: it answers ABSTAIN without an id, and hands out no number. */
    CHECK_EQ_INT(ask_score(g, t0 + 2, 0.9, 0.9), 0);
    CHECK_EQ_INT(ask_score(g, t0 + 2, 0.9, 0.1), 0);
    CHECK(g->next_seq == TOP && g->stats.full == 2 && g->stats.abstained == 5);
    CHECK_EQ_INT(g->npending, 3);
    CHECK(check_copy(g));
    /* What it recorded is answered: the mark is the last number, which no
     * request will ever have. */
    CHECK_EQ_INT(gateLearn(g, t0 + 3, g->epoch, TOP - 2, 1), 1);
    CHECK(g->answer_mark == TOP);
    CHECK(check_copy(g));
    CHECK_EQ_INT(gateLearn(g, t0 + 3, g->epoch, TOP - 3, 1), 1);
    CHECK(g->candidate != NULL && g->candidate->samples == 2 && g->npending == 1);
    CHECK_EQ_INT(gateLearn(g, t0 + 3, g->epoch, TOP, 1), 0);
    CHECK(check_copy(g));
    /* Or given up (a lost answer: an answer has arrived since). */
    gateMaintain(g, t0 + 1 + SIM_TIMEOUT);
    CHECK(g->npending == 0 && g->stats.missing == 1 && g->missing_run == 0 && gateFull(g));
    CHECK(check_copy(g));
    /* Neither a recovery nor a reset hands numbers back. */
    gateRecover(g, t0 + 5000, 99);
    CHECK(g->next_seq == TOP && gateFull(g));
    CHECK_EQ_INT(ask_score(g, t0 + 5001, 0.9, 0.9), 0);
    CHECK(check_copy(g));
    gateReset(g, t0 + 6000, 100);
    CHECK(g->next_seq == TOP && gateFull(g));
    CHECK_EQ_INT(ask_score(g, t0 + 6001, 0.9, 0.9), 0);
    check_round_trip(g, 38);
}

/* The last snapshot numbers. Generation 2^62 is the last one: when it is
 * gone, the gate tests nothing any more. */
TEST(snapshot_numbers_run_out) {
    gate *g = new_gate(GATE_KIND_SCORE, 0);
    int64_t t0 = g->created_ms;
    uint64_t id;

    g->generations = TOP - 1;
    g->candidate->gen = TOP - 1;
    g = through_codec(g);
    if (g == NULL) return;
    CHECK(g->candidate != NULL && g->candidate->gen == TOP - 1);
    /* The bar of a generation so late is 32 * g * (g + 1) / DELTA = 3.4e39
     * (at 2^62), which is e^91.03: it is still a number. */
    CHECK_NEAR(g->candidate->log_threshold, 2 * 62 * log(2.0) + log(32.0) - log(0.2), 1e-9);
    id = ask_score(g, t0 + 1, 0.9, 0.9);
    CHECK(id != 0 && gateFindObservation(g, id)->cand_gen == TOP - 1);
    CHECK(check_copy(g));
    /* A recovery drops it: the next snapshot is the last. */
    gateRecover(g, t0 + 10, 7);
    CHECK(g->generations == TOP && g->candidate != NULL && g->candidate->gen == TOP);
    CHECK_NEAR(g->candidate->log_threshold, 2 * 62 * log(2.0) + log(32.0) - log(0.2), 1e-9);
    id = ask_score(g, t0 + 11, 0.9, 0.9);
    CHECK(id != 0 && gateFindObservation(g, id)->cand_gen == TOP);
    CHECK(gateState(g) == GATE_STATE_CALIBRATING);
    CHECK(check_copy(g));
    /* And when that one is dropped there is none. A score gate that tests
     * nothing and answers nothing records nothing. */
    gateRecover(g, t0 + 20, 8);
    CHECK(g->generations == TOP && g->candidate == NULL && g->serving == NULL && !g->blind);
    CHECK(gateState(g) == GATE_STATE_COLD);
    CHECK_EQ_INT(gateMaintenanceDue(g, t0 + 21), 0);
    CHECK_EQ_INT(ask_score(g, t0 + 21, 0.9, 0.9), 0);
    CHECK(check_copy(g));
    gateReset(g, t0 + 30, 9);
    CHECK(g->generations == TOP && g->candidate == NULL);
    check_round_trip(g, 39);
}

/* The same for a classifier, which has learned enough for a snapshot and
 * has no number left for one: it goes on learning. */
TEST(snapshot_numbers_run_out_classifier) {
    gate *g = classifier_learning(40);
    sim s = {41, g->now_ms, 0};

    g->generations = TOP;
    g = through_codec(g);
    if (g == NULL) return;
    teach(&s, g, 60);
    CHECK(g->generations == TOP && g->candidate == NULL && g->serving == NULL);
    CHECK(g->trainer.updates >= 60 && g->snapshot_updates == 0);
    CHECK(gateState(g) == GATE_STATE_COLD);
    CHECK_EQ_INT(gateMaintenanceDue(g, s.now), 0);
    check_round_trip(g, 40);
}

/* The end of the clock. Past INT64_MAX - GATE_MAX_FEEDBACK_TIMEOUT_MS the
 * gate's clock stays where it is, whatever time it is handed: nothing gets
 * older and nothing times out, but everything else goes on. */
TEST(clock_at_its_end) {
    gate *g = new_gate(GATE_KIND_SCORE, 0);
    uint64_t first, second, third;

    g->created_ms = g->now_ms = g->candidate->frozen_ms = LAST_MS - 100;
    g = through_codec(g);
    if (g == NULL) return;
    first = ask_score(g, LAST_MS - 99, 0.9, 0.9);
    second = ask_score(g, LAST_MS, 0.9, 0.9);
    CHECK(first != 0 && second != 0 && g->now_ms == LAST_MS);
    CHECK(check_copy(g));
    gateMaintain(g, INT64_MAX);
    CHECK_EQ_INT(g->now_ms, LAST_MS);
    third = ask_score(g, INT64_MAX, 0.9, 0.9);
    CHECK(third != 0 && gateFindObservation(g, third)->created_ms == LAST_MS);
    CHECK_EQ_INT(g->now_ms, LAST_MS);
    CHECK_EQ_INT(g->npending, 3);
    CHECK_EQ_INT(gateMaintenanceDue(g, INT64_MAX), 0);
    CHECK(check_copy(g));
    /* An answer waits for the older ones, for ever: the first request is
     * 99 ms old and stays 99 ms old. */
    CHECK_EQ_INT(gateLearn(g, INT64_MAX, g->epoch, third, 1), 1);
    CHECK(g->nanswered == 1 && g->candidate->samples == 0 && g->now_ms == LAST_MS);
    CHECK(check_copy(g));
    CHECK_EQ_INT(gateLearn(g, INT64_MAX - 1, g->epoch, first, 1), 1);
    CHECK_EQ_INT(gateLearn(g, LAST_MS + 1, g->epoch, second, 0), 1);
    CHECK(g->npending == 0 && g->candidate->samples == 3 && g->candidate->errors == 1);
    gatePause(g, INT64_MAX);
    gateResume(g, INT64_MAX);
    CHECK_EQ_INT(g->now_ms, LAST_MS);
    CHECK(check_copy(g));
    /* A reset and a recovery set the clock: to a value in its range. */
    gateReset(g, INT64_MAX, 5);
    CHECK_EQ_INT(g->now_ms, LAST_MS);
    CHECK(g->candidate != NULL && g->candidate->frozen_ms == LAST_MS);
    CHECK(check_copy(g));
    gateRecover(g, INT64_MIN, 6);
    CHECK_EQ_INT(g->now_ms, 0);
    CHECK(g->candidate != NULL && g->candidate->frozen_ms == 0);
    CHECK(ask_score(g, -5, 0.9, 0.9) != 0 && g->newest->created_ms == 0);
    check_round_trip(g, 41);
}

/* A score gate made by hand that holds one generation, number 1, with ten
 * samples and its widest region proven: it serves, or it is the candidate.
 * (With an ALPHA of 0.9 a drift detector has room for many disagreements:
 * each multiplies its likelihood ratios by 0.95 / 0.9.) */
static gate *made_score_gate(double alpha, int serving) {
    gateGeneration *gen;
    gateConfig cfg;
    char err[160] = "";
    gate *g;

    gateConfigInit(&cfg);
    cfg.alpha = alpha;
    cfg.delta = 0.2;
    cfg.audit = 1.0;
    cfg.feedback_timeout_ms = SIM_TIMEOUT;
    MUST(gateConfigValidate(&cfg, err, sizeof(err)) == GATE_OK);
    g = gateAllocate(&cfg);
    g->lineage = 3;
    g->epoch = 4;
    g->created_ms = g->now_ms = 1700000000000ll;
    g->generations = 1;
    gen = gateGenerationAllocate(g);
    gen->gen = 1;
    gen->samples = 10;
    gen->regions[0].n = 10;
    gen->regions[0].certified_at = 10;
    gen->policy[0] = 0;
    gen->log_threshold = gateBar(g, 1);
    if (serving) {
        gen->promoted_ms = g->now_ms;
        g->serving = gen;
    } else {
        g->candidate = gen;
    }
    return g;
}

/* A request of a gate made by hand that names generation 1 as the one that
 * served: a spot check with this number, or (number 0) proof for it as the
 * candidate. */
static void add_request(gate *g, uint64_t spot) {
    gateObs *o = new_request(g->next_seq++, g->now_ms,
                             spot ? GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_SERV
                                  : GATE_OBS_CAND);

    if (spot) {
        o->serv_gen = 1;
        o->serv_score = 0.5;
    } else {
        o->cand_gen = 1;
        o->cand_score = 0.5;
    }
    o->spot = spot;
    gateObsLink(g, o);
}

/* The ways in which a gate that had decoded did not go through the codec
 * again, as this round of tests found them, each with the shortest history
 * that shows it. Whether the decoder refuses such a blob, or the core and
 * the encoder cope with what becomes of it, is not what is asked here: if
 * it decodes, what the core makes of it must close. */
TEST(closed_whatever_decodes) {
    long failures = closure_failures;
    int decoded = 0;
    gate *g, *d;
    gateObs *o;
    blob b;

    /* 1. The request counters at their limit, and one more request: they
     * are written as the limit, and still add up. */
    g = new_gate(GATE_KIND_SCORE, 0);
    g->stats.requests = TOP;
    g->stats.abstained = TOP - 5;
    g->stats.audited = 5;
    d = through_codec(g);
    if (d != NULL) {
        CHECK(ask_score(d, d->now_ms + 1, 0.5, 0.9) != 0);
        CHECK(d->stats.requests == TOP + 1 && d->stats.abstained == TOP - 4);
        CHECK(closes(d, "with one request more than the limit"));
        g = clone(d);
        CHECK(g != NULL && g->stats.requests == TOP);
        CHECK(g != NULL && g->stats.audited + g->stats.abstained == TOP && g->stats.accepted == 0);
        gateFree(g);
        gateFree(d);
        decoded++;
    }

    /* 2. One request number is left, and it is used: the gate is full for
     * good, and holds a request. */
    g = new_gate(GATE_KIND_SCORE, 0);
    g->next_seq = TOP - 1;
    d = through_codec(g);
    if (d != NULL) {
        CHECK(ask_score(d, d->now_ms + 1, 0.5, 0.9) == TOP - 1);
        CHECK(d->next_seq == TOP && d->npending == 1 && gateFull(d));
        CHECK(closes(d, "with the last request number used"));
        CHECK(check_copy(d));
        gateFree(d);
        decoded++;
    }

    /* 3. A drift detector that has heard as many disagreements as it has
     * asked spot checks, and a spot check that waits and disagrees too ... */
    g = made_score_gate(0.9, 1);
    g->serving->drift[0].asked = 3;
    g->serving->drift[0].errors = 3;
    add_request(g, 2);
    d = through_codec(g);
    if (d != NULL) {
        CHECK_EQ_INT(gateLearn(d, d->now_ms + 1, d->epoch, 1, 0), 1);
        CHECK(d->serving != NULL && d->serving->audit_errors == 1);
        CHECK(closes(d, "after a disagreement more than spot checks"));
        gateFree(d);
        decoded++;
    }
    /* ... and two spot checks that wait with the same number, and both
     * disagree. */
    g = made_score_gate(0.9, 1);
    g->serving->drift[0].asked = 1;
    add_request(g, 1);
    add_request(g, 1);
    d = through_codec(g);
    if (d != NULL) {
        CHECK_EQ_INT(gateLearn(d, d->now_ms + 1, d->epoch, 1, 0), 1);
        CHECK_EQ_INT(gateLearn(d, d->now_ms + 1, d->epoch, 2, 0), 1);
        CHECK(d->serving != NULL && d->serving->audit_errors == 2);
        CHECK(closes(d, "after two disagreements of one spot check"));
        gateFree(d);
        decoded++;
    }

    /* 4. A request that claims to be proof for the candidate through a
     * guess of a label that is never answered, while another generation
     * serves: when its answer arrives, the core must not count it in the
     * candidate's paired table and not in its samples. */
    g = classifier_two_generations();
    o = new_request(g->next_seq++, g->now_ms, GATE_OBS_AUDIT | GATE_OBS_CAND);
    o->cand_gen = 2;
    o->cand_label = 2;
    o->cand_score = 0.9;
    o->serv_gen = 1;
    o->serv_label = 0;
    o->serv_score = 0.9;
    gateObsLink(g, o);
    b = encode(g, 0);
    d = gateDecode(b.p, b.n, NULL, 0);
    if (d != NULL) {
        CHECK_EQ_INT(gateLearn(d, d->now_ms + 1, d->epoch, d->newest->seq, 0), 1);
        CHECK(closes(d, "after proof through a label that is never answered"));
        gateFree(d);
        decoded++;
    }
    blob_free(&b);
    gateFree(g);

    /* 5. A spot check that names the candidate as the generation it was a
     * spot check of (with a number the candidate's detector has not
     * handed out, and with one it has), and the candidate takes over. */
    for (uint64_t asked = 0; asked <= 5; asked += 5) {
        g = made_score_gate(0.2, 0);
        g->candidate->drift[0].asked = asked;
        add_request(g, 0);
        add_request(g, 5);
        g->newest->flags &= ~(uint32_t)GATE_OBS_SERV; /* kept, and proof for nothing */
        b = encode(g, 0);
        d = gateDecode(b.p, b.n, NULL, 0);
        if (d != NULL) {
            CHECK_EQ_INT(gateLearn(d, d->now_ms + 1, d->epoch, 1, 1), 1);
            CHECK(d->serving != NULL && d->serving->gen == 1 && d->candidate == NULL);
            CHECK(d->npending == 1 && d->newest->spot == 5);
            CHECK(closes(d, "after the candidate that a spot check names took over"));
            gateFree(d);
            decoded++;
        }
        blob_free(&b);
        gateFree(g);
    }
    /* (Two of them are refused as they are: the proof through a label
     * that is never answered, and the spot check number the candidate's
     * detector has not handed out.) */
    CHECK_EQ_INT(decoded, 5);
    CHECK_EQ_INT(closure_failures, failures);
}

/* A gate with everything near its limits: 'g' is moved there in memory,
 * with every relation between its numbers kept (what was equal stays
 * equal, what was smaller stays smaller), and taken through the codec.
 * 'counters' is the room left to every counter, 'numbers' to the request
 * numbers, 'snapshots' to the snapshot numbers and 'ms' to the clock.
 * Releases 'g'; returns the decoded gate. */
static gate *to_the_limits(gate *g, uint64_t counters, uint64_t numbers, uint64_t snapshots,
                           int64_t ms)
{
    uint64_t shift, mark = g->answer_mark;
    int64_t dt = LAST_MS - ms - g->now_ms;
    gateCounters *s = &g->stats;

    /* Counters with nothing to be compared with. */
    s->full = s->observations = s->learned = s->ignored = s->missing = s->dropped =
        s->promotions = s->revocations = s->retired = TOP - counters;
    /* The requests are the sum of three. */
    s->accepted += TOP - counters - s->requests;
    s->requests = TOP - counters;
    /* The examples: the trainer's, those at its last snapshot, and those
     * of the snapshots. */
    if (g->cfg.kind == GATE_KIND_CLASSIFIER) {
        shift = TOP - counters - g->trainer.updates;
        g->trainer.updates += shift;
        g->snapshot_updates += shift;
        if (g->candidate) g->candidate->model.updates += shift;
        if (g->serving) g->serving->model.updates += shift;
    }
    for (int serving = 0; serving < 2; serving++) {
        gateGeneration *gen = serving ? g->serving : g->candidate;

        if (gen == NULL) continue;
        /* Samples, with the regions and the paired table of the rows that
         * are tested: all by the same amount. */
        shift = TOP - counters - gen->samples;
        gen->samples += shift;
        for (uint32_t r = 0; r < g->nrows; r++) {
            if (!gateRowTested(g, r)) continue;
            for (uint32_t t = 0; t < GATE_THRESHOLDS; t++) {
                gen->regions[r * GATE_THRESHOLDS + t].n += shift;
                gen->paired[r * GATE_THRESHOLDS + t] += shift;
            }
        }
        shift = TOP - counters - gen->audits;
        gen->audits += shift;
        gen->audit_errors += shift;
        gen->missed = TOP - counters;
        /* (With that many missed requests a candidate has used up any
         * budget: half of the time it gets one without a limit.) */
        if (gen->budget) gen->budget = counters % 2 ? 0 : TOP;
        /* Spot checks, with the disagreements and the numbers of the ones
         * that wait. The blocks begin elsewhere now: they start empty. */
        for (uint32_t r = 0; r < g->nrows; r++) {
            if (gen->drift[r].asked == 0) continue;
            shift = TOP - counters - gen->drift[r].asked;
            gen->drift[r].asked += shift;
            gen->drift[r].errors += shift;
            memset(gen->drift[r].wrong, 0, sizeof(gen->drift[r].wrong));
            memset(gen->drift[r].lost, 0, sizeof(gen->drift[r].lost));
            for (gateObs *o = g->oldest; o; o = o->next) {
                uint32_t row = g->cfg.kind == GATE_KIND_CLASSIFIER ? (uint32_t)o->serv_label : 0;

                if (o->spot && o->serv_gen == gen->gen && row == r) o->spot += shift;
            }
        }
    }
    /* Snapshot numbers, also where a request names one. */
    shift = TOP - snapshots - g->generations;
    g->generations += shift;
    if (g->candidate) g->candidate->gen += shift;
    if (g->serving) g->serving->gen += shift;
    for (gateObs *o = g->oldest; o; o = o->next) {
        if (o->cand_gen) o->cand_gen += shift;
        if (o->serv_gen) o->serv_gen += shift;
    }
    /* Request numbers, and the mark between them; the clock and the time
     * of every request. */
    shift = TOP - numbers - g->next_seq;
    g->next_seq += shift;
    if (mark) g->answer_mark += shift;
    for (gateObs *o = g->oldest; o; o = o->next) {
        o->seq += shift;
        o->created_ms += dt;
    }
    g->now_ms += dt;

    return through_codec(g);
}

/* Gates of each kind near all their limits live through a long random
 * history: counters are counted beyond 2^62, the request numbers and the
 * snapshot numbers run out, the clock arrives at its end and is handed
 * times beyond. After every operation (every few for the larger blobs)
 * the gate must close. Every so often the history goes on with the copy:
 * what was beyond its limit in memory is then the limit. */
TEST(limits_on_the_way) {
    static const struct {
        int kind;
        uint32_t k;
        int every;
        uint64_t counters, numbers, snapshots;
        int64_t ms;
    } run[] = {
        /* The counters go beyond, the snapshot numbers run out and the
         * clock ends; there are request numbers enough. */
        {GATE_KIND_SCORE, 0, 1, 3, (uint64_t)1 << 40, 3, 20000},
        {GATE_KIND_CLASSIFIER, 2, 5, 3, (uint64_t)1 << 40, 4, 20000},
        /* The request numbers run out early, and nothing else ends. */
        {GATE_KIND_SCORE, 0, 1, 2, 40, (uint64_t)1 << 40, (int64_t)1 << 40},
        {GATE_KIND_CLASSIFIER, 2, 5, (uint64_t)1 << 40, 60, (uint64_t)1 << 40, (int64_t)1 << 40},
        /* Only the counters go beyond. */
        {GATE_KIND_CLASSIFIER, 3, 9, 2, (uint64_t)1 << 40, (uint64_t)1 << 40, (int64_t)1 << 40},
        /* The request numbers run out late, after the counters. */
        {GATE_KIND_SCORE, 0, 1, 300, 600, (uint64_t)1 << 40, (int64_t)1 << 40},
        /* Everything ends. */
        {GATE_KIND_SCORE, 0, 1, 3, 60, 2, 20000},
    };
    long steps = fuzz_count / 40 < 2500 ? 2500 : fuzz_count / 40;
    long failures = closure_failures, checked = closure_checks;
    int beyond = 0, numbers = 0, snapshots = 0, clock = 0, swapped = 0;

    for (size_t i = 0; i < COUNTOF(run); i++) {
        gate *g = run[i].kind == GATE_KIND_SCORE ? score_with_requests(71 + i, 1)
                                                 : classifier_with_everything(71 + i, run[i].k);
        int counted = 0;
        driver d;

        g = to_the_limits(g, run[i].counters, run[i].numbers, run[i].snapshots, run[i].ms);
        if (g == NULL) continue;
        CHECK(closes(g, "near its limits"));
        driver_init(&d, g, 81 + i + 1000 * fuzz_seed);
        for (long step = 0; step < steps && closure_failures < failures + 3; step++) {
            driver_step(&d, g);
            /* The client of drive() recovers and resets rarely: here it
             * happens every few hundred operations, and each time uses
             * up a snapshot number. */
            if (step % 401 == 400 || step % 1501 == 1500) {
                if (step % 401 == 400) gateRecover(g, d.s.now, 0x7265636f766572ull + (uint64_t)step);
                else gateReset(g, d.s.now, 0x7265736574ull + (uint64_t)step);
                d.n = 0;
            }
            if (g->stats.requests > TOP && g->stats.learned > TOP) counted = 1;
            if (step % run[i].every) continue;
            if (!closes(g, "on the way past its limits"))
                printf("  ... after operation %ld of history %zu\n", step, i);
            /* The limits the core keeps. */
            MUST(g->next_seq <= TOP && g->generations <= TOP && g->answer_mark <= g->next_seq);
            MUST(g->now_ms >= 0 && g->now_ms <= LAST_MS);
            /* Every so often the copy takes the place of the original. */
            if (step % 97 == 96) {
                gate *copy = clone(g);

                MUST(copy != NULL);
                if (copy != NULL) {
                    gateFree(g);
                    g = copy;
                    swapped++;
                }
            }
        }
        beyond += counted;
        if (g->next_seq == TOP) numbers++;
        if (g->generations == TOP) snapshots++;
        if (g->now_ms == LAST_MS) clock++;
        gateFree(g);
    }
    printf("  %ld states near the limits: %d histories counted beyond 2^62, %d ran out of "
           "request numbers, %d of snapshot numbers, %d reached the end of the clock\n",
           closure_checks - checked, beyond, numbers, snapshots, clock);
    CHECK_EQ_INT(closure_failures, failures);
    CHECK(beyond >= 4 && numbers >= 3 && snapshots >= 3 && clock >= 3 && swapped > 100);
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
 * weights, where one is like the next, and 'fine' those that end inside
 * the regions, the paired table and the drift detectors of a generation. */
static void check_truncations(gate *g, size_t stride, size_t fine) {
    blob b = encode(g, 0);
    layout l = layout_of(g);
    size_t tried = 0, accepted = 0, other = 0;

    for (size_t n = 0; n < b.n; n++) {
        unsigned char *prefix;
        char err[160] = "";
        gate *copy;

        if (n >= 8 && n % stride != 0 && in_weights(g, &l, n - 8) && in_weights(g, &l, n + 8))
            continue;
        if (n >= 8 && n % fine != 0 && in_regions(g, &l, n - 8) && in_regions(g, &l, n + 8))
            continue;
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
    CHECK(tried >= b.n / (stride > fine ? stride : fine));
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
    uint64_t audited, plain, id[601];
    before was;

    check_truncations(golden_score_gate(), 1, 1);
    check_truncations(golden_blind_gate(), 1, 1);
    check_truncations(score_with_requests(31, 0), 1, 1);
    check_truncations(score_with_late_answers(), 1, 1);
    check_truncations(score_blind(&audited, &plain), 1, 1);
    check_truncations(new_gate(GATE_KIND_CLASSIFIER, 2), 1, 1);
    check_truncations(classifier_learning(32), 1, 1);
    check_truncations(classifier_with_everything(33, 2), 509, 509);
    check_truncations(classifier_blind(34, &was), 127, 127);
    check_truncations(golden_classifier_gate(1), 8191, 8191);
    /* Spot checks that wait, disagreed and were lost; the answer mark in
     * the middle of 41 requests; a candidate with its paired table (every
     * prefix that ends in the regions, the table or the detectors of its
     * two generations); spot checks of a generation that is gone; a gate
     * near all its limits. */
    check_truncations(score_with_spot_checks(3, id), 1, 1);
    check_truncations(score_short_of_blind(), 1, 1);
    check_truncations(score_with_mark_in_the_middle(), 1, 1);
    check_truncations(classifier_with_paired_table(), 4099, 1);
    check_truncations(classifier_with_stale_spot_checks(3), 8191, 61);
    check_truncations(to_the_limits(score_with_requests(43, 1), 3, 40, 2, 30000), 1, 1);
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
    uint64_t audited, plain, id[601];
    before was;

    check_allocation_failures(golden_score_gate());
    check_allocation_failures(golden_blind_gate());
    check_allocation_failures(score_with_late_answers());
    check_allocation_failures(score_blind(&audited, &plain));
    check_allocation_failures(classifier_with_everything(41, 2));
    check_allocation_failures(classifier_blind(42, &was));
    check_allocation_failures(golden_classifier_gate(1));
    check_allocation_failures(score_with_spot_checks(3, id));
    check_allocation_failures(score_with_mark_in_the_middle());
    check_allocation_failures(classifier_with_paired_table());
    check_allocation_failures(classifier_with_stale_spot_checks(3));
    check_allocation_failures(to_the_limits(classifier_with_everything(44, 3), 3, 40, 2, 30000));
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
    long spots;       /* with spot checks waiting for their answer */
    long stale;       /* with spot checks of a generation that is gone */
    long wrong;       /* answering, with disagreements in an open block */
    long lost;        /* answering, with lost answers in an open block */
    long paired;      /* with a candidate that has a paired table */
    long missed;      /* with a candidate that has missed requests */
    long middle;      /* with the answer mark between waiting requests */
    long both_ways;   /* ... while a run of missing answers is under way */
} tally;

static void look_at(const gate *g, tally *t, int *was_blind) {
    waiting w = count_waiting(g);
    int wrong = 0, lost = 0;

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
    if (w.spots) t->spots++;
    if (w.stale) t->stale++;
    for (uint32_t r = 0; g->serving && r < g->nrows; r++) {
        for (int s = 0; s < GATE_DRIFT_SCALES; s++) {
            if (g->serving->drift[r].wrong[s]) wrong = 1;
            if (g->serving->drift[r].lost[s]) lost = 1;
        }
    }
    t->wrong += wrong;
    t->lost += lost;
    if (g->candidate && paired_any(g, g->candidate)) t->paired++;
    if (g->candidate && g->candidate->missed) t->missed++;
    if (w.before > 0 && w.before < (int)g->npending) t->middle++;
    if (w.before > 0 && g->missing_run) t->both_ways++;
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
    tally all;

    memset(&all, 0, sizeof(all));
    for (size_t i = 0; i < COUNTOF(run); i++) {
        gate *g = new_gate(run[i].kind, run[i].k);
        int was_blind = 0, bad = 0;
        driver d;
        tally t;

        memset(&t, 0, sizeof(t));
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
        /* Each kind of gate got into the states of late and lost answers,
         * with the last answer in the middle of what waits; and with a
         * label that is never answered candidates miss requests. */
        CHECK(t.answered > 0 && t.provisional > 0 && t.run > 0 && t.middle > 0);
        if (run[i].k == 3) CHECK(t.missed > 0);
        /* With the seed of every ordinary run each kind also went blind
         * and saw again, and had spot checks waiting. The first two had
         * disagreements and lost answers in the blocks of a detector (the
         * third, with a label it never answers and a stricter one, answers
         * too rarely for that), and the classifiers compared a candidate
         * with the generation that serves. (Other seeds take the three
         * histories elsewhere: what they must all get to, taken together,
         * is checked below.) */
        if (fuzz_seed == 0) {
            CHECK(t.blind > 0 && t.recorded > 0 && t.seeing > 0 && t.spots > 0);
            if (run[i].k < 3) CHECK(t.wrong > 0 && t.lost > 0);
            if (run[i].k >= 2) CHECK(t.paired > 0);
        }
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
        all.spots += t.spots;
        all.stale += t.stale;
        all.wrong += t.wrong;
        all.lost += t.lost;
        all.paired += t.paired;
        all.missed += t.missed;
        all.middle += t.middle;
        all.both_ways += t.both_ways;
        gateFree(g);
    }
    printf("  %ld states: %ld with answers waiting, %ld with requests passed by (%ld for "
           "nothing), %ld in a run of missing answers, %ld blind (%ld with requests), %ld "
           "seeing again, %ld with two generations\n",
           all.checked, all.answered, all.provisional, all.gone, all.run, all.blind,
           all.recorded, all.seeing, all.both);
    printf("  %ld with spot checks waiting (%ld of a generation that is gone), %ld with "
           "disagreements and %ld with lost answers in an open block, %ld with a paired "
           "table, %ld with missed requests, %ld with the answer mark between waiting "
           "requests (%ld of them in a run of missing answers)\n",
           all.spots, all.stale, all.wrong, all.lost, all.paired, all.missed, all.middle,
           all.both_ways);
    CHECK(all.checked >= 3 * steps / 11 && all.both > 0);
    CHECK(all.blind > 0 && all.recorded > 0 && all.seeing > 0);
    CHECK(all.spots > 0 && all.wrong > 0 && all.lost > 0 && all.paired > 0 && all.missed > 0);
}

/* A burst of requests without answers, at chosen moments of a history: the
 * gate fills up (the first burst meets a gate that tests its first
 * generation, where every request is proof); a third of the answers arrive
 * after all, newest first, and wait; proof gives up waiting and moves on
 * past thousands of requests at once; and then their time is up. They are
 * lost answers, thousands of them, and no outage: answers have arrived
 * since they were asked. Then a burst that nobody answers at all: that is
 * an outage, and the gate goes blind. */
TEST(bursts_on_the_way) {
    gate *g = new_gate_with(GATE_KIND_SCORE, 0, 600000);
    int was_blind = 0, bad = 0;
    waiting w;
    driver d;
    tally t;

    memset(&t, 0, sizeof(t));
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
         * 1365 answers wait behind the second. Every request that waits
         * was asked before the last answer arrived. */
        if (round == 0) CHECK(w.answered == 1365 && w.unanswered == 2730 && w.provisional == 0);
        if (round == 0) CHECK(g->answer_mark == g->next_seq && w.before == 4095);
        /* A quarter of the feedback timeout later proof moves on. */
        d.s.now += 150000;
        gateMaintain(g, d.s.now);
        if (!check_copy(g)) bad++;
        look_at(g, &t, &was_blind);
        w = count_waiting(g);
        if (round == 0) CHECK(w.answered == 0 && w.unanswered == 0 && w.provisional == 2730);
        if (round == 0) CHECK_EQ_INT(g->nevidence, 0);
        /* And then the answers that never came are given up: lost, all
         * 2730 of them, and the gate tests on. */
        d.s.now += 450000;
        gateMaintain(g, d.s.now);
        if (!check_copy(g)) bad++;
        look_at(g, &t, &was_blind);
        if (round == 0) {
            CHECK(!g->blind && g->npending == 0 && g->missing_run == 0);
            CHECK_EQ_INT(g->stats.missing, 2730);
            CHECK(g->candidate != NULL && g->candidate->samples == GATE_MAX_PENDING);
            CHECK(gateState(g) == GATE_STATE_CALIBRATING);
        }
        /* A burst that gets no answer at all, and its time is up: the
         * first 32 are a run of missing answers, the gate goes blind and
         * the others are proof for nothing (it keeps its snapshot number
         * for the day it sees again). */
        for (int i = 0; i < 5000; i++) {
            if (i % 5 == 0) d.s.now++;
            sim_ask_now(&d.s, g);
        }
        if (round == 0) CHECK(gateFull(g) && count_waiting(g).before == 0);
        if (!check_copy(g)) bad++;
        d.s.now += 600000;
        gateMaintain(g, d.s.now);
        if (!check_copy(g)) bad++;
        look_at(g, &t, &was_blind);
        if (round == 0) {
            CHECK(g->blind && g->npending == 0 && g->candidate == NULL);
            CHECK_EQ_INT(g->stats.missing, 2730 + GATE_MISSING_RUN);
            CHECK_EQ_INT(g->stats.dropped, GATE_MAX_PENDING - GATE_MISSING_RUN);
            CHECK_EQ_INT(g->generations, 1);
        }
        for (int step = 0; step < 300; step++) driver_step(&d, g);
        if (!check_copy(g)) bad++;
    }
    MUST(bad == 0);
    CHECK(t.full > 0 && t.answered > 0 && t.provisional > 0 && t.blind > 0);
    gateFree(g);
}

/* ---------------------------- Random corruption -------------------------- */

static long fuzz_planned = 0, fuzz_accepted = 0, fuzz_rejected = 0;

/* Field-sized runs that numbers tend to be wrong in: extremes, infinities
 * and NaN as a double and as two floats, a weight just over the limit, one
 * bucket too many, the limit of the counters, one less and one more, the
 * lengths of the blocks of spot checks and one more, and as two 32-bit
 * numbers: the longest run of missing answers and one more, which is also
 * the flag of a request the proof has moved on without, alone and with an
 * answer, and the length of a block. */
static const uint64_t fuzz_runs[] = {
    0, ~0ull, 1, 0x7fffffffffffffffull, 0x8000000000000000ull, 0xffffffff00000000ull,
    0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000000ull,
    0x7f8000007f800000ull, 0x7fc000007fc00000ull, 0x4280000142800001ull,
    0x0000200000002000ull, 0x4000000000000000ull, 0x4000000000000001ull,
    0x3fffffffffffffffull, 64, 65, 512, 513, 4096, 4097,
    0x0000001f0000001full, 0x0000002000000020ull, 0x0000003000000030ull,
    0x0000002400000024ull, 0x0000001400000014ull, 0x0000004000000040ull,
};

/* 'count' corruptions of the encoding of 'g': one byte, one bit, a few
 * bytes anywhere, a run from the list above, or a small number (half of
 * these two where a counter is). Each must be refused with a reason, or
 * give a gate that encodes back to exactly the corrupted bytes, can be
 * used, and closes after it was used: every one of them, one in eight
 * after every step of a longer life (see exercise()), and one in five
 * hundred throughout two thousand operations of a client. Releases 'g'. */
static void corrupt(gate *g, const char *name, long count, uint64_t seed) {
    static size_t counters[MAX_COUNTERS];
    blob b = encode(g, 0);
    layout l = layout_of(g);
    size_t ncounters = counter_offsets(g, &l, counters);
    unsigned char *buf = malloc(b.n); /* exactly as long: overreads show */
    uint64_t rng = seed + 1000 * fuzz_seed;
    int reported = 0;

    fuzz_planned += count;
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
            if (kind >= 6 && j == 0 && (r >> 40) % 2)
                pos = counters[gate_test_rand64(&rng) % ncounters];
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
            long before = closure_failures;
            blob again = encode(copy, 0);

            MUST(again.n == b.n && memcmp(again.p, buf, b.n) == 0);
            blob_free(&again);
            /* One in five hundred lives long instead. */
            if (fuzz_accepted % 500 == 499) live_long(copy, r, 2000);
            else exercise(copy, r, fuzz_accepted % 8 == 0);
            if (closure_failures != before && reported++ < 3) {
                printf("  ... %s, corruption %ld:", name, i);
                for (int j = 0; j < n; j++) printf(" byte %zu = %02x", at[j], buf[at[j]]);
                printf("\n");
            }
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

#define CORRUPT(gate, percent, seed) corrupt(gate, #gate, fuzz_count * (percent) / 100, seed)

TEST(random_corruptions) {
    long failures = closure_failures, checked = closure_checks;
    uint64_t audited, plain, id[601];
    before was;

    CORRUPT(golden_score_gate(), 8, 51);
    CORRUPT(score_about_to_go_blind(), 3, 62);
    CORRUPT(golden_blind_gate(), 6, 52);
    CORRUPT(score_with_requests(52, 0), 6, 53);
    CORRUPT(score_with_requests(53, 1), 8, 54);
    CORRUPT(score_with_late_answers(), 8, 55);
    CORRUPT(score_blind(&audited, &plain), 5, 56);
    CORRUPT(score_seeing_again(), 5, 57);
    /* Spot checks that wait, disagreed and were lost, in blocks of all
     * lengths; a detector one disagreement short of the alarm and one a
     * lost answer short of giving up; the last answer in the middle of
     * the waiting requests; a gate near all its limits. */
    CORRUPT(score_with_spot_checks(3, id), 10, 63);
    CORRUPT(score_short_of_the_alarm(), 5, 64);
    CORRUPT(score_short_of_blind(), 5, 65);
    CORRUPT(score_with_mark_in_the_middle(), 5, 66);
    CORRUPT(to_the_limits(score_with_requests(57, 1), 3, 40, 2, 30000), 8, 67);
    CORRUPT(classifier_learning(54), 3, 58);
    CORRUPT(classifier_with_late_answers(55), 3, 59);
    CORRUPT(classifier_blind(56, &was), 2, 60);
    CORRUPT(golden_classifier_gate(1), 3, 61);
    /* A candidate with its paired table and missed requests; spot checks
     * of a generation that is gone next to those of one that serves; a
     * classifier near its limits. */
    CORRUPT(classifier_with_paired_table(), 3, 68);
    CORRUPT(classifier_with_stale_spot_checks(3), 2, 69);
    CORRUPT(to_the_limits(classifier_with_everything(58, 3), 2, (uint64_t)1 << 40, 3, 30000), 2, 70);
    printf("  %ld corruptions: %ld refused, %ld decoded and used, the closure property "
           "checked %ld times\n",
           fuzz_accepted + fuzz_rejected, fuzz_rejected, fuzz_accepted, closure_checks - checked);
    /* Each of them was tried (the shares above are rounded down, by less
     * than one corruption for each of the twenty blobs). */
    CHECK_EQ_INT(fuzz_accepted + fuzz_rejected, fuzz_planned);
    CHECK(fuzz_planned > fuzz_count - 20);
    /* Both happen a lot: a corruption that goes unnoticed (a counter, a
     * weight) and one that is caught. (With a few hundred corruptions for
     * twenty blobs "a lot" is not a number to count on.) */
    if (fuzz_count >= 2000) CHECK(fuzz_accepted * 10 > fuzz_count && fuzz_rejected * 10 > fuzz_count);
    /* Whatever decoded and was used went through the codec again. */
    CHECK_EQ_INT(closure_failures, failures);
    CHECK(closure_checks - checked >= fuzz_accepted);
}

/* ------------------------------ Random states -----------------------------
 * A corrupted byte mostly gives a blob that is refused, or one that
 * differs from a state of the core in a single number. This campaign makes
 * up states instead: a gate is changed in memory in a few places at once,
 * by somebody who knows what the fields are and what the decoder asks of
 * them (and steps over that now and then), and encoded. Most of the
 * results decode: states the core would never leave behind, which the
 * decoder lets in all the same. Each is used like every hostile blob that
 * decodes, and must close. */

static long made_up_planned = 0, made_up = 0, made_up_decoded = 0;

/* A number that matters to a counter: small, around the length of a block
 * of spot checks, or at the limit. */
static uint64_t some_count(uint64_t *rng) {
    static const uint64_t notable[] = {
        0, 1, 2, 3, 31, 32, 63, 64, 65, 511, 512, 513, 4095, 4096, 4097,
        TOP - 4097, TOP - 65, TOP - 64, TOP - 2, TOP - 1, TOP,
    };
    uint64_t r = gate_test_rand64(rng);

    return r % 3 == 0 ? (r >> 8) % 100 : notable[(r >> 8) % COUNTOF(notable)];
}

/* The number of a generation: none, one of the two the gate holds, or
 * another one it has frozen. */
static uint64_t some_generation(const gate *g, uint64_t *rng) {
    uint64_t r = gate_test_rand64(rng);

    switch (r % 5) {
    case 0: return 0;
    case 1: return g->candidate ? g->candidate->gen : 0;
    case 2: return g->serving ? g->serving->gen : 0;
    case 3: return g->generations;
    default: return g->generations ? 1 + (r >> 8) % g->generations : 0;
    }
}

/* The number of a spot check: none, one the detector of the request's row
 * has handed out (the last, the first, the one of another request), one it
 * has not, or the last there is. */
static uint64_t some_spot(const gate *g, const gateObs *o, uint64_t *rng) {
    uint64_t r = gate_test_rand64(rng), asked = 0;
    uint32_t row = g->cfg.kind == GATE_KIND_CLASSIFIER ? (uint32_t)o->serv_label : 0;

    if (g->serving && row < g->nrows) asked = g->serving->drift[row].asked;
    switch (r % 7) {
    case 0: return 0;
    case 1: return 1;
    case 2: return asked;
    case 3: return asked + 1;
    case 4: return asked ? 1 + (r >> 8) % asked : 0;
    case 5: return g->oldest->spot ? g->oldest->spot : g->newest->spot;
    default: return TOP;
    }
}

static void make_up_request(gate *g, gateObs *o, uint64_t *rng) {
    int classifier = g->cfg.kind == GATE_KIND_CLASSIFIER;
    uint64_t r = gate_test_rand64(rng), k = classifier ? g->cfg.nclasses : 0;

    switch (r % 13) {
    case 0: o->flags = (uint32_t)((r >> 8) % 64); break;
    case 1: o->flags ^= 1u << ((r >> 8) % 6); break;
    case 2: o->cand_label = (int32_t)((r >> 8) % (k + 1)) - 1; break;
    case 3: o->serv_label = (int32_t)((r >> 8) % (k + 1)) - 1; break;
    case 4: o->cand_gen = some_generation(g, rng); break;
    case 5: o->serv_gen = some_generation(g, rng); break;
    case 6:
        /* An answer, with or without the flag that goes with it. */
        o->result = (int32_t)((r >> 8) % (classifier ? k : 2));
        if ((r >> 16) % 4) o->flags |= GATE_OBS_RESOLVED;
        if ((r >> 20) % 4) gateSparseFree(&o->x);
        break;
    case 7:
        o->result = -1;
        if ((r >> 16) % 4) o->flags &= ~(uint32_t)GATE_OBS_RESOLVED;
        break;
    case 8: o->spot = some_spot(g, o, rng); break;
    case 9:
        /* A spot check of the generation that serves, or of another. */
        o->flags |= GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT;
        if ((r >> 8) % 2) o->flags |= GATE_OBS_SERV;
        if (g->serving && (r >> 12) % 4) {
            o->serv_gen = g->serving->gen;
            o->serv_label = classifier ? (int32_t)((r >> 16) % k) : -1;
        }
        o->spot = some_spot(g, o, rng);
        break;
    case 10: o->created_ms = (int64_t)((uint64_t)g->now_ms - (r >> 8) % 5000); break;
    case 11: o->cand_score = gate_test_uniform(rng); break;
    default: o->serv_score = (r >> 8) % 4 ? gate_test_uniform(rng) : g->grid[(r >> 12) % 32];
    }
}

static void make_up_generation(gate *g, gateGeneration *gen, uint64_t *rng) {
    uint64_t r = gate_test_rand64(rng), shift;
    uint32_t row = (uint32_t)((r >> 8) % g->nrows), t = (uint32_t)((r >> 16) % GATE_THRESHOLDS);
    gateRegion *reg = &gen->regions[row * GATE_THRESHOLDS + t];
    gateDrift *d = &gen->drift[row];

    switch (r % 15) {
    case 0: gen->samples = some_count(rng); break;
    case 1: gen->errors = some_count(rng); break;
    case 2: gen->missed = some_count(rng); break;
    case 3: gen->budget = some_count(rng); break;
    case 4:
        gen->audits = some_count(rng);
        gen->audit_errors = (r >> 24) % 2 ? gen->audits : gen->audits / 2;
        break;
    case 5:
        /* Everything it counts as samples, by the same amount: near the
         * limit, with every relation kept. */
        if (gen->samples >= TOP) break;
        shift = TOP - (r >> 24) % 4 - gen->samples;
        gen->samples += shift;
        for (uint32_t i = 0; i < g->nrows * GATE_THRESHOLDS; i++) {
            if (!gateRowTested(g, i / GATE_THRESHOLDS)) continue;
            gen->regions[i].n += shift;
            gen->paired[i] += shift;
        }
        break;
    case 6: reg->logw[(r >> 24) % GATE_NBETS] = (r >> 28) % 4 ? 40.0 * gate_test_uniform(rng) : DBL_MAX; break;
    case 7: reg->certified_at = (r >> 24) % 2 ? reg->n : 0; break;
    case 8: reg->errors = (r >> 24) % 2 ? reg->n : 0; break;
    case 9:
        /* A row of the paired table: nested, and within the samples or
         * one beyond. */
        shift = gen->samples + ((r >> 24) % 8 == 0);
        for (uint32_t i = 0; i < GATE_THRESHOLDS; i++) {
            gen->paired[row * GATE_THRESHOLDS + i] = shift;
            if (gate_test_rand64(rng) % 4 == 0 && shift) shift -= 1 + gate_test_rand64(rng) % shift;
        }
        break;
    case 10:
        /* A detector at a place of its own, with no more disagreements
         * and lost answers than its open blocks hold (or one more). */
        d->asked = some_count(rng);
        d->errors = (r >> 24) % 3 ? d->asked : d->asked / 2;
        for (int s = 0; s < GATE_DRIFT_SCALES; s++) {
            uint32_t open = gateDriftOpen(d, s) + ((r >> 28) % 16 == 0);

            d->wrong[s] = open ? (uint32_t)(gate_test_rand64(rng) % (open + 1)) : 0;
            d->lost[s] = (r >> 32) % 2 ? open - d->wrong[s] : 0;
        }
        break;
    case 11: d->errors = (r >> 24) % 2 ? d->asked : d->asked + 1; break;
    case 12: gen->gen = some_generation(g, rng); break;
    case 13:
        for (int s = 0; s < GATE_DRIFT_SCALES; s++) d->wrong[s] = d->lost[s] = 0;
        break;
    default: gen->promoted_ms = (r >> 24) % 2 ? 0 : g->now_ms;
    }
}

static void make_up_ledger(gate *g, uint64_t *rng) {
    uint64_t r = gate_test_rand64(rng), last = g->newest ? g->newest->seq : 0, top = 0;
    gateCounters *s = &g->stats;
    gateGeneration *gen;
    gateObs *o;

    switch (r % 15) {
    case 0: g->missing_run = (uint32_t)((r >> 8) % 33); break;
    case 1: g->answer_mark = (r >> 8) % (g->next_seq + 2); break;
    case 2:
        /* The next request number: just behind the last request, near
         * the limit, or at it. */
        g->next_seq = (r >> 8) % 3 == 0 ? last + 1 + (r >> 12) % 3 : TOP - (r >> 12) % 3;
        if (g->next_seq <= last) g->next_seq = last + 1;
        if (g->answer_mark > g->next_seq || (r >> 16) % 2) g->answer_mark = g->next_seq;
        break;
    case 3:
        /* The snapshot numbers: the largest in use, near the limit, at it. */
        if (g->candidate && g->candidate->gen > top) top = g->candidate->gen;
        if (g->serving && g->serving->gen > top) top = g->serving->gen;
        for (o = g->oldest; o; o = o->next) {
            if (o->cand_gen > top) top = o->cand_gen;
            if (o->serv_gen > top) top = o->serv_gen;
        }
        g->generations = (r >> 8) % 3 == 0 ? top : TOP - (r >> 12) % 3;
        if (g->generations < top) g->generations = top;
        break;
    case 4: g->blind = !g->blind; break;
    case 5: g->paused = !g->paused; break;
    case 6:
        /* The clock near its end, or at it: every time moves along. */
        if (g->now_ms >= LAST_MS - 100000) break;
        top = (uint64_t)(LAST_MS - (int64_t)((r >> 8) % 3 ? (r >> 12) % 3000 : 0) - g->now_ms);
        g->now_ms += (int64_t)top;
        for (o = g->oldest; o; o = o->next) o->created_ms += (int64_t)top;
        break;
    case 7:
        /* The request counters near the limit: they still add up. */
        top = TOP - (r >> 8) % 4;
        if (s->requests >= top) break;
        if ((r >> 12) % 2) s->accepted += top - s->requests; else s->abstained += top - s->requests;
        s->requests = top;
        break;
    case 8:
        s->full = s->observations = s->learned = s->ignored = s->missing = s->dropped =
            s->promotions = s->revocations = s->retired = TOP - (r >> 8) % 3;
        break;
    case 9:
        if (g->cfg.kind != GATE_KIND_CLASSIFIER) break;
        g->trainer.updates = some_count(rng);
        g->snapshot_updates = (r >> 8) % 2 ? g->trainer.updates : some_count(rng);
        break;
    case 10:
        /* The candidate serves and the serving generation is tested. */
        gen = g->candidate;
        g->candidate = g->serving;
        g->serving = gen;
        if (g->candidate) g->candidate->promoted_ms = 0;
        break;
    case 11:
        if ((r >> 8) % 2) {
            gateGenerationRelease(g->candidate);
            g->candidate = NULL;
        } else {
            gateGenerationRelease(g->serving);
            g->serving = NULL;
        }
        break;
    case 12:
        /* One more request, the same as the newest but for its number,
         * if the numbers allow. */
        if (g->newest == NULL || g->next_seq >= TOP || g->npending >= 64) break;
        o = new_request(g->next_seq++, g->now_ms, g->newest->flags);
        o->cand_label = g->newest->cand_label;
        o->serv_label = g->newest->serv_label;
        o->cand_score = g->newest->cand_score;
        o->serv_score = g->newest->serv_score;
        o->cand_gen = g->newest->cand_gen;
        o->serv_gen = g->newest->serv_gen;
        o->result = g->newest->result;
        o->spot = g->newest->spot;
        gateObsLink(g, o);
        break;
    case 13:
        s->window = (r >> 8) % (GATE_COVERAGE_WINDOW + 1);
        s->coverage = (r >> 24) % 3 ? gate_test_uniform(rng) : (double)((r >> 28) % 2);
        break;
    default:
        g->lineage = r;
        g->epoch = ~r;
    }
}

/* 'count' made-up states from 'g': each a copy with one to six changes.
 * Releases 'g'. */
static void make_up(gate *g, const char *name, long count, uint64_t seed) {
    uint64_t rng = seed + 1000 * fuzz_seed;
    int reported = 0;

    made_up_planned += count;
    for (long i = 0; i < count; i++) {
        gate *x = clone(g), *copy;
        int changes = 1 + (int)(gate_test_rand64(&rng) % 6);
        long failures = closure_failures;
        uint64_t before = rng;
        char err[160] = "";
        blob b, again;

        if (x == NULL) break;
        for (int c = 0; c < changes; c++) {
            uint64_t r = gate_test_rand64(&rng);
            gateGeneration *gen = (r >> 8) % 2 ? x->candidate : x->serving;

            if (r % 10 < 5 && x->npending) {
                gateObs *o = x->oldest;

                for (uint64_t skip = (r >> 16) % x->npending; skip > 0; skip--) o = o->next;
                make_up_request(x, o, &rng);
            } else if (r % 10 < 8 && (gen || x->candidate || x->serving)) {
                make_up_generation(x, gen ? gen : x->candidate ? x->candidate : x->serving, &rng);
            } else {
                make_up_ledger(x, &rng);
            }
        }
        b = encode(x, 0);
        gateFree(x);
        made_up++;
        copy = gateDecode(b.p, b.n, err, sizeof(err));
        if (copy == NULL) {
            MUST(err[0] != '\0');
        } else {
            made_up_decoded++;
            again = encode(copy, 0);
            MUST(blob_equal(&b, &again));
            blob_free(&again);
            /* One in two hundred lives long instead. */
            if (made_up_decoded % 200 == 0) live_long(copy, before, 2000);
            else exercise(copy, before, made_up_decoded % 4 == 0);
            if (closure_failures != failures && reported++ < 3)
                printf("  ... %s, made-up state %ld (random state %llu, %d changes)\n", name, i,
                       (unsigned long long)before, changes);
            gateFree(copy);
        }
        blob_free(&b);
    }
    gateFree(g);
}

#define MAKE_UP(gate, percent, seed) make_up(gate, #gate, fuzz_count / 4 * (percent) / 100, seed)

TEST(random_states) {
    long failures = closure_failures, checked = closure_checks;
    uint64_t id[601];

    MAKE_UP(golden_score_gate(), 14, 91);
    MAKE_UP(golden_blind_gate(), 4, 92);
    MAKE_UP(score_with_requests(93, 0), 8, 93);
    MAKE_UP(score_with_late_answers(), 10, 94);
    MAKE_UP(score_with_spot_checks(3, id), 14, 95);
    MAKE_UP(score_short_of_the_alarm(), 8, 96);
    MAKE_UP(score_short_of_blind(), 8, 97);
    MAKE_UP(to_the_limits(score_with_requests(98, 1), 3, 40, 2, 30000), 10, 98);
    MAKE_UP(golden_classifier_gate(1), 8, 99);
    MAKE_UP(classifier_with_paired_table(), 8, 100);
    MAKE_UP(classifier_with_stale_spot_checks(3), 4, 101);
    MAKE_UP(to_the_limits(classifier_with_everything(102, 3), 2, (uint64_t)1 << 40, 3, 30000), 4, 102);
    printf("  %ld made-up states: %ld decoded and used, the closure property checked %ld "
           "times\n", made_up, made_up_decoded, closure_checks - checked);
    /* Each of them was made (a quarter as many as corruptions, shared out
     * over twelve gates and rounded down). */
    CHECK_EQ_INT(made_up, made_up_planned);
    CHECK(made_up_planned > fuzz_count / 4 - 12);
    /* Most of them get in, and many are refused. */
    if (made_up >= 500)
        CHECK(made_up_decoded * 4 > made_up && (made_up - made_up_decoded) * 10 > made_up);
    CHECK_EQ_INT(closure_failures, failures);
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
    RUN(score_answer_mark);

    RUN(score_spot_checks);
    RUN(score_one_disagreement_short_of_the_alarm);
    RUN(score_one_lost_answer_short_of_blind);
    RUN(classifier_paired_and_missed);
    RUN(classifier_spot_checks_of_a_generation_that_is_gone);

    RUN(widest_classifier);
    RUN(full_of_requests);
    RUN(full_of_clues);
    RUN(full_of_answers);
    RUN(full_of_both);
    RUN(full_again);

    RUN(hostile_header);
    RUN(previous_layouts);
    RUN(truth_values);
    RUN(hostile_config);
    RUN(hostile_ledger_and_models);
    RUN(policy_follows_the_regions);
    RUN(hostile_requests);
    RUN(hostile_answer_mark);
    RUN(hostile_paired_table);
    RUN(invariants_of_the_new_fields);
    RUN(paired_table_at_the_serving_policy);
    RUN(hostile_drift_detectors);
    RUN(hostile_spot_checks);
    RUN(request_flags);
    RUN(hostile_counters);
    RUN(unusual_but_valid);
    RUN(every_counter_is_bounded);
    RUN(every_counter_is_written_within_its_limit);
    RUN(request_counters_beyond_the_limit);
    RUN(request_numbers_run_out);
    RUN(snapshot_numbers_run_out);
    RUN(snapshot_numbers_run_out_classifier);
    RUN(clock_at_its_end);
    RUN(closed_whatever_decodes);
    RUN(error_buffer);
    RUN(truncated_blobs);
    RUN(allocation_failures);
    RUN(no_allocation_on_credit);
    RUN(no_requests_on_credit);
    RUN(every_state_on_the_way);
    RUN(bursts_on_the_way);
    RUN(limits_on_the_way);
    RUN(random_corruptions);
    RUN(random_states);
    closure_report();
    return TEST_REPORT();
}
