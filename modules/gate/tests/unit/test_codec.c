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
 *   - Two reference blobs (a score gate and a classifier gate in which
 *     every kind of field has a value of its own) were written by a
 *     separate encoder: a Python script that follows the layout comment of
 *     gate_codec.c with struct.pack and never runs the C code. The same two
 *     gates are built here by hand. gateEncode() must produce those bytes
 *     and gateDecode() of those bytes must produce those gates. The numbers
 *     checked next to them (bars, thresholds, the lowest proven region)
 *     follow from the rules of gate_core.h, by hand or from that script.
 *   - layout_of() derives where every section of a blob starts from the
 *     layout comment alone. Every blob encoded here must have that size, a
 *     digest must differ from the plain encoding in exactly the bytes the
 *     header names, and hostile blobs are patched at those offsets.
 *
 * The rest drives gates through the real API into every state of their
 * life, takes them through the codec and compares state and behaviour.
 * Then it attacks the decoder: hostile blobs made by hand, every
 * truncation, failing allocations, random corruption. A corrupted blob
 * that decodes all the same must also be safe to use: it is given to the
 * core to work on.
 *
 * Usage: test_codec [number of random corruptions]. The default is 200000;
 * a smaller number keeps a run under valgrind short.
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
#define LEDGER_BYTES 49     /* five 64-bit fields, the pause flag, one more */
#define GENERATION_BYTES 72 /* nine 64-bit fields */
#define REGION_BYTES 56     /* four log wealths, three counters */
#define DRIFT_BYTES 32      /* two log statistics, two counters */
#define REQUEST_BYTES 68    /* without its clues, eight bytes each */
#define COUNTERS_BYTES 120  /* fifteen 64-bit fields */

/* Offsets inside the ledger and inside a request. */
#define LEDGER_NOW 24
#define LEDGER_PAUSED 40
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

/* One GPREDICT. The teacher is right 97 times in 100 (classifier), or
 * agrees with every cheap result that scored at least 0.5 and with half of
 * the others (score gate). */
static asked sim_ask(sim *s, gate *g) {
    int classifier = g->cfg.kind == GATE_KIND_CLASSIFIER;
    gateSparse x = {0, NULL, NULL};
    double score = 0.0, u;
    gateEval ev;
    asked a;

    s->now += 1 + (int64_t)(gate_test_rand64(&s->rng) % 20);
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

static const char *const sim_labels[] = {"yes", "no", "maybe"};

/* A gate that gets through its whole life in a few hundred requests. With
 * three classes it also has a stricter and a deferred label. */
static gate *new_gate(int kind, uint32_t k) {
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
    cfg.feedback_timeout_ms = 2000;
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
 * are only kept for training, with their clues. */
static void leave_waiting(sim *s, gate *g) {
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
}

typedef struct waiting {
    int unanswered; /* evidence without its answer */
    int answered;   /* evidence with its answer, waiting for its turn */
    int training;   /* requests that are not evidence */
    int clues;      /* requests that hold clues */
} waiting;

static waiting count_waiting(const gate *g) {
    waiting w = {0, 0, 0, 0};

    for (const gateObs *o = g->oldest; o; o = o->next) {
        if (o->flags & GATE_OBS_RESOLVED) w.answered++;
        else if (o->flags & GATE_OBS_EVIDENCE) w.unanswered++;
        else w.training++;
        if (o->x.n) w.clues++;
    }
    return w;
}

#define DRIVE_IDS 64

/* A client that does a bit of everything, and the same for any two gates
 * that behave the same. Most requests get their answer at once; the others
 * later, usually oldest first, and now and then never. It also sends ids
 * that are stale or unknown, teaches (GADD), pauses for a while, falls
 * silent until everything has timed out, meets a teacher that turned
 * against the gate and, rarely, resets or recovers. Returns a digest of
 * every reply it got. */
static uint64_t drive(gate *g, uint64_t seed, int nops) {
    int classifier = g->cfg.kind == GATE_KIND_CLASSIFIER;
    uint64_t answers = classifier ? g->cfg.nclasses : 2;
    uint64_t ids[DRIVE_IDS], seen = seed;
    int32_t say[DRIVE_IDS];
    sim s = {seed, g->now_ms, 0};
    int n = 0;

    /* What waits already is this client's to answer too. */
    for (const gateObs *o = g->oldest; o && n < DRIVE_IDS; o = o->next) {
        if (o->flags & GATE_OBS_RESOLVED) continue;
        ids[n] = o->seq;
        say[n++] = (int32_t)(o->seq % answers);
    }
    for (int i = 0; i < nops; i++) {
        uint64_t op = gate_test_rand64(&s.rng) % 100, r = gate_test_rand64(&s.rng);
        uint64_t reply = 0;

        s.now += 1 + (int64_t)(r % 16);
        if (op < 55 || (op < 85 && n == 0)) {
            asked a = sim_ask(&s, g);

            reply = (uint64_t)a.res.action + 8 * (uint64_t)(a.res.label + 1) + 1024 * a.res.seq;
            if (a.res.seq && r % 4) {
                reply += 4 * (uint64_t)gateLearn(g, s.now, g->epoch, a.res.seq, a.answer);
            } else if (a.res.seq && n < DRIVE_IDS) {
                ids[n] = a.res.seq;
                say[n++] = a.answer;
            }
        } else if (op < 85) {
            /* An answer that is due; or, one time in eight, no answer ever. */
            int j = r % 4 ? 0 : (int)((r >> 8) % (uint64_t)n);

            if (r % 8) reply = (uint64_t)gateLearn(g, s.now, g->epoch, ids[j], say[j]);
            n--;
            memmove(&ids[j], &ids[j + 1], sizeof(ids[0]) * (size_t)(n - j));
            memmove(&say[j], &say[j + 1], sizeof(say[0]) * (size_t)(n - j));
        } else if (op < 88) {
            reply = (uint64_t)gateLearn(g, s.now, g->epoch ^ (r >> 8 & 1),
                                        r % (g->next_seq + 2), (int32_t)(r % answers));
        } else if (op < 93 && classifier) {
            uint32_t c = (uint32_t)(r % answers);
            gateSparse x;

            make_clues(&s.rng, c, &x);
            reply = gateTrain(g, s.now, &x, (int32_t)c) == GATE_OK;
            gateSparseFree(&x);
        } else if (op == 94 && r % 4 == 0) {
            s.now += g->cfg.feedback_timeout_ms + (int64_t)(r % 1000);
        } else if (op == 95 && (g->paused || r % 4 == 0)) {
            if (g->paused) gateResume(g, s.now); else gatePause(g, s.now);
        } else if (op == 96 && (s.wrong || r % 8 == 0)) {
            s.wrong = !s.wrong;
        } else if (op == 97 && r % 64 == 0) {
            gateReset(g, s.now, r);
            n = 0;
        } else if (op == 97 && r % 64 == 1) {
            gateRecover(g, s.now, r);
            n = 0;
        } else {
            gateMaintain(g, s.now);
        }
        seen = (seen ^ reply) * 0x100000001b3ull + (uint64_t)gateState(g);
    }
    return seen;
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

/* Everything a gate must survive, whatever state it is in. Releases 'g'. */
static void check_round_trip(gate *g, uint64_t seed) {
    char err[160] = "";
    layout l = layout_of(g);
    blob plain, digest, again, other;
    gate *copy, *third;

    CHECK(gateCheckInvariants(g, err, sizeof(err)));
    g->runtime_tag = 0x7461676765647461ull;
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
 * The text form of the two blobs written by the Python reference encoder:
 * hex bytes, and '*N' for N zero bytes. */

#define GOLDEN_SCORE_NOW 73
#define GOLDEN_SCORE_COUNTERS 2145

static const char golden_score_text[] =
    "4741544501000100019a9999999999c93f7b14ae47e17a843f000000000000f0 "
    "3f00000000e80300000100000000000000080706050403020118171615141312 "
    "11feffffffffffffff0068e5cf8b01000003 *18 "
    "010300000000000000f040e5cf8b0100007854e5cf8b01000032000000000000 "
    "0002 *15 060000000000000001 *21 "
    "e03f000000000000f03f000000000000f83f0000000000000040320000000000 "
    "0000020000000000000029 *13 "
    "d0bf000000000000d03f000000000000e83f000000000000f43f1e *1677 08c0 "
    "*30 "
    "c03f000000000000d8bf06000000000000000100000000000000090000000000 "
    "00000200000007000000000000001864e5cf8b0100000b000000ffffffffffff "
    "ffff *14 e43f *8 "
    "0300000000000000ffffffff0000000008000000000000000c66e5cf8b010000 "
    "19000000ffffffffffffffff *14 a03f *8 "
    "0300000000000000010000000000000064000000000000003c00000000000000 "
    "0a000000000000001e00000000000000040000000000000064 *13 "
    "e83f28000000000000001e000000000000000200000000000000010000000000 "
    "00000300000000000000010000000000000002 *15";

#define GOLDEN_CLASSIFIER_NOW 129
#define GOLDEN_CLASSIFIER_COUNTERS 306549

static const char golden_classifier_text[] =
    "4741544501000100009a9999999999b93f9a9999999999a93f000000000000d0 "
    "3f03000000040000007370616d0300000068616d0a00000068c3a96c6c6f20e2 "
    "9c939a9999999999b93f9a9999999999a93f9a9999999999b93f000001140000 "
    "00e093040000000000887766554433221100ffeeddccbbaa990068e5cf8b0100 "
    "00404ae7cf8b0100000700000000000000012800000000000000012d *10 3f "
    "*32762 80be0000c03f *65530 "
    "80420000003e000080c200000040010700000000000000a0eee6cf8b01 *10 "
    "0c00000000000000030000000000000005 *23 300200000000000028 *13 80bf "
    "*65530 403f *32775 "
    "bf9a9999999999b93f9a9999999999c93f333333333333d33f9a9999999999d9 "
    "3f0c0000000000000003 *21 "
    "f83f00000000000004400000000000000c400000000000001240090000000000 "
    "0000010000000000000007 *1719 02 *3647 "
    "010500000000000000502be6cf8b0100006052e6cf8b0100005a000000000000 "
    "0004 *15 0b0000000000000002 *15 14 *32774 40 *65538 803f *8 "
    "9a9999999999b9bf9a9999999999c9bf333333333333d3bf9a9999999999d9bf "
    "0a0000000000000001 *1783 500000000000000003 *47 4f *55 4e *55 4d "
    "*55 4c *15 32 *39 4b *55 4a *15 46 *13 "
    "224000000000000023400000000000002440000000000000254049 *15 3d "
    "*3181 f43f000000000000e0bf0b0000000000000002 *39 "
    "0b00000000000000040000000200000000000000e86be5cf8b01000000000000 "
    "ffffffffffffffff *32 "
    "ffffffff0200000003000000a00f00000000803f000080bf0300000000000000 "
    "b015e7cf8b0100000d00000001 *13 "
    "e83f000000000000e03f07000000000000000500000000000000ffffffff0300 "
    "00000500000064000000ff1f00000000803f000000bf0000803e040000000000 "
    "0000b115e7cf8b0100001f0000000000000001 *9 "
    "ec3f000000000000ee3f07000000000000000500000000000000020000000000 "
    "00000900000000000000404ae7cf8b010000020000000200000001 *9 "
    "dc3f000000000000ef3f07000000000000000500000000000000ffffffff01 *10 "
    "c0e803000000000000bc020000000000006400000000000000c8000000000000 "
    "0007 *8 02 *12 "
    "e03f2c01000000000000fa000000000000000400000000000000050000000000 "
    "0000060000000000000002000000000000000100000000000000030000000000 "
    "0000";

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

/* What the core computes when it freezes a generation or a region crosses. */
static void finish_generation(const gate *g, gateGeneration *gen) {
    for (uint32_t r = 0; r < g->nrows; r++) gateGenerationRefreshPolicy(g, gen, r);
    gen->log_threshold = gateBudgetLogThreshold(g->cfg.delta, gen->gen,
                                                gateTestedRows(g) * gateEntryPoints(g));
}

/* The score gate of the first reference blob. It answers (generation 3
 * serves, its widest region is proven), nothing is being tested, and two
 * spot checks wait: one for its answer, one behind it with its answer. */
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
    cfg.feedback_timeout_ms = 1;
    MUST(gateConfigValidate(&cfg, err, sizeof(err)) == GATE_OK);
    g = gateAllocate(&cfg);
    g->lineage = 0x0102030405060708ull;
    g->epoch = 0x1112131415161718ull;
    g->created_ms = -2;
    g->now_ms = 1700000000000ll;
    g->generations = 3;

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
    finish_generation(g, s);

    g->next_seq = 9;
    o = gateObsAllocate();
    o->seq = 7;
    o->created_ms = 1699999999000ll;
    o->flags = GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_SERV;
    o->serv_score = 0.625;
    o->serv_gen = 3;
    gateObsLink(g, o);
    o = gateObsAllocate();
    o->seq = 8;
    o->created_ms = 1699999999500ll;
    o->flags = GATE_OBS_AUDIT | GATE_OBS_SERV | GATE_OBS_RESOLVED;
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
 * generation 7 being tested, and four waiting requests: one kept for
 * training from before any snapshot, an unanswered piece of evidence, an
 * answered one behind it, and one recorded while paused. */
static gate *golden_classifier_gate(void) {
    static const char *const labels[] = {"spam", "ham", "h\xc3\xa9llo \xe2\x9c\x93"};
    static const size_t lens[] = {4, 3, 10};
    static const uint32_t idx2[] = {3, 4000}, idx3[] = {5, 100, 8191}, idx9[] = {0};
    static const float val2[] = {1.0f, -1.0f}, val3[] = {1.0f, -0.5f, 0.25f}, val9[] = {-2.0f};
    gateConfig cfg;
    char err[160] = "";
    gateGeneration *c, *s;
    gateObs *o;
    gate *g;

    gateConfigInit(&cfg);
    cfg.kind = GATE_KIND_CLASSIFIER;
    cfg.alpha = 0.1;
    cfg.delta = 0.05;
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
    c->frozen_ms = 1700000100000ll;
    c->samples = 12;
    c->errors = 3;
    c->paired = 5;
    c->budget = 560;
    MUST(gateModelInit(&c->model, GATE_DIM, 3) == GATE_OK);
    c->model.updates = 40;
    c->model.w[1] = -1.0f;
    c->model.w[16384] = 0.75f;
    c->model.b[2] = -0.5f;
    c->regions[0] = (gateRegion){{0.1, 0.2, 0.3, 0.4}, 12, 3, 0};
    c->regions[1] = (gateRegion){{1.5, 2.5, 3.5, 4.5}, 9, 1, 7};
    c->regions[32].n = 2;
    finish_generation(g, c);

    s = g->serving = gateGenerationAllocate(g);
    s->gen = 5;
    s->frozen_ms = 1700000050000ll;
    s->promoted_ms = 1700000060000ll;
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
    finish_generation(g, s);

    g->next_seq = 11;
    o = gateObsAllocate();
    o->seq = 2;
    o->created_ms = 1700000001000ll;
    set_clues(&o->x, 2, idx2, val2);
    gateObsLink(g, o);
    o = gateObsAllocate();
    o->seq = 3;
    o->created_ms = 1700000110000ll;
    o->flags = GATE_OBS_AUDIT | GATE_OBS_CAND | GATE_OBS_SERV;
    o->cand_label = 1;
    o->serv_label = 0;
    o->cand_score = 0.75;
    o->serv_score = 0.5;
    o->cand_gen = 7;
    o->serv_gen = 5;
    set_clues(&o->x, 3, idx3, val3);
    gateObsLink(g, o);
    o = gateObsAllocate();
    o->seq = 4;
    o->created_ms = 1700000110001ll;
    o->flags = GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_EVIDENCE | GATE_OBS_RESOLVED;
    o->cand_label = 0;
    o->serv_label = 1;
    o->cand_score = 0.875;
    o->serv_score = 0.9375;
    o->cand_gen = 7;
    o->serv_gen = 5;
    o->result = 2;
    gateObsLink(g, o);
    o = gateObsAllocate();
    o->seq = 9;
    o->created_ms = 1700000123456ll;
    o->flags = GATE_OBS_WOULD_ACCEPT;
    o->cand_label = 2;
    o->serv_label = 1;
    o->cand_score = 0.4375;
    o->serv_score = 0.96875;
    o->cand_gen = 7;
    o->serv_gen = 5;
    set_clues(&o->x, 1, idx9, val9);
    gateObsLink(g, o);

    g->stats = (gateCounters){
        .requests = 1000, .accepted = 700, .audited = 100, .abstained = 200, .full = 7,
        .window = 512, .coverage = 0.5, .observations = 300, .learned = 250, .ignored = 4,
        .missing = 5, .dropped = 6, .promotions = 2, .revocations = 1, .retired = 3,
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

TEST(reference_score_gate) {
    gate *g = golden_score_gate(), *d;
    blob want = golden(golden_score_text), got = encode(g, 0), again;
    layout l = layout_of(g);
    char err[160] = "";

    CHECK_EQ_INT(want.n, 2265);
    CHECK_EQ_INT(l.ledger + LEDGER_NOW, GOLDEN_SCORE_NOW);
    CHECK_EQ_INT(l.counters, GOLDEN_SCORE_COUNTERS);
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

        /* A few of the fields by name, in case both the builder above and
         * the decoder got one wrong in the same way. */
        CHECK_EQ_INT(d->cfg.kind, GATE_KIND_SCORE);
        CHECK(d->cfg.alpha == 0.2 && d->cfg.delta == 0.01 && d->cfg.audit == 1.0);
        CHECK_EQ_INT(d->cfg.feedback_timeout_ms, 1);
        CHECK(d->lineage == 0x0102030405060708ull && d->epoch == 0x1112131415161718ull);
        CHECK_EQ_INT(d->created_ms, -2);
        CHECK_EQ_INT(d->now_ms, 1700000000000ll);
        CHECK(d->candidate == NULL && d->trainer.w == NULL);
        CHECK_EQ_INT(d->serving->regions[0].certified_at, 41);
        CHECK(d->serving->regions[31].logw[3] == -3.0);
        CHECK(d->serving->drift[0].logw[1] == -0.375);
        CHECK_EQ_INT(d->npending, 2);
        CHECK_EQ_INT(d->nevidence, 2);
        CHECK_EQ_INT(d->pending_bytes, 2 * sizeof(gateObs));
        CHECK(gateFindObservation(d, 7) == d->oldest && d->oldest == d->ehead);
        CHECK(gateFindObservation(d, 8) == d->newest && d->newest == d->etail);
        CHECK_EQ_INT(d->newest->result, 1);
        CHECK(d->newest->serv_score == 0.03125);
        CHECK_EQ_INT(d->stats.revocations, 2);
        CHECK(d->stats.coverage == 0.75);

        /* What is not stored. The bar is log(3 * 4) + log(1 * 32) -
         * log(0.01); the widest region crossed and every region of a score
         * gate is an entry point, so the gate answers from score 0 up. */
        CHECK_NEAR(d->serving->log_threshold, 10.555812738575817, 1e-12);
        CHECK_EQ_INT(d->serving->policy[0], 0);
        CHECK(gateRowThreshold(d, 0) == 0.0);
        CHECK(gateState(d) == GATE_STATE_ACTIVE);
        gateFree(d);
    }
    blob_free(&got);
    blob_free(&want);
    gateFree(g);
}

TEST(reference_classifier_gate) {
    gate *g = golden_classifier_gate(), *d;
    blob want = golden(golden_classifier_text), got = encode(g, 0), again;
    layout l = layout_of(g);
    char err[160] = "";

    CHECK_EQ_INT(want.n, 306669);
    CHECK_EQ_INT(l.ledger + LEDGER_NOW, GOLDEN_CLASSIFIER_NOW);
    CHECK_EQ_INT(l.counters, GOLDEN_CLASSIFIER_COUNTERS);
    CHECK_EQ_INT(l.end, want.n);
    check_same_bytes(&got, &want);

    d = decode(&want);
    CHECK(d != NULL);
    if (d != NULL) {
        const gateObs *o = gateFindObservation(d, 3);

        CHECK(gateCheckInvariants(d, err, sizeof(err)));
        CHECK_EQ_STR(gate_diff(g, d), "");
        again = encode(d, 0);
        check_same_bytes(&again, &want);
        blob_free(&again);

        CHECK_EQ_INT(d->cfg.nclasses, 3);
        CHECK_EQ_STR(d->cfg.labels[0], "spam");
        CHECK_EQ_STR(d->cfg.labels[1], "ham");
        CHECK_EQ_STR(d->cfg.labels[2], "h\xc3\xa9llo \xe2\x9c\x93");
        CHECK(d->cfg.class_alpha[0] == 0.1 && d->cfg.class_alpha[1] == 0.05);
        CHECK(!d->cfg.class_defer[0] && !d->cfg.class_defer[1] && d->cfg.class_defer[2]);
        CHECK_EQ_INT(d->paused, 1);
        CHECK_EQ_INT(d->snapshot_updates, 40);
        CHECK_EQ_INT(d->trainer.updates, 45);
        CHECK(d->trainer.w[8191] == -0.25f && d->trainer.w[8192] == 1.5f);
        CHECK(d->trainer.w[24575] == 64.0f && d->trainer.b[1] == -64.0f);
        CHECK(d->candidate->model.w[16384] == 0.75f && d->candidate->model.b[2] == -0.5f);
        CHECK(d->serving->model.w[8191] == 2.0f && d->serving->model.b[0] == 1.0f);
        CHECK_EQ_INT(d->candidate->budget, 560);
        CHECK_EQ_INT(d->serving->regions[32 + 7].n, 73);
        CHECK(d->serving->drift[1].logw[0] == 1.25);
        CHECK_EQ_INT(d->npending, 4);
        CHECK_EQ_INT(d->nevidence, 2);
        CHECK_EQ_INT(d->pending_bytes, 4 * sizeof(gateObs) + 6 * 8);
        CHECK(o != NULL && o == d->ehead && o->x.n == 3);
        CHECK(o && o->x.idx[2] == 8191 && o->x.val[1] == -0.5f && o->cand_score == 0.75);
        CHECK_EQ_INT(d->newest->seq, 9);
        CHECK(d->newest->x.val[0] == -2.0f);

        /* What is not stored. Two tested rows with four entry points each:
         * bar = log(g (g + 1)) + log(8) - log(0.05). In row 1 of the
         * serving generation region 7 crossed and is an entry point, 6
         * crossed and leans on 7, 5 did not cross, so 4 (crossed, no entry
         * point) proves nothing: the row answers from threshold 6, which
         * is 1 - (2/3) * (0.001 / (2/3)) ^ (6/31). The candidate crossed
         * region 1 only, which is no entry point. */
        CHECK_NEAR(d->candidate->log_threshold, 9.1005255059689762, 1e-12);
        CHECK_NEAR(d->serving->log_threshold, 8.4763711968959825, 1e-12);
        CHECK_EQ_INT(d->serving->policy[0], -1);
        CHECK_EQ_INT(d->serving->policy[1], 6);
        CHECK_EQ_INT(d->serving->policy[2], -1);
        for (uint32_t r = 0; r < 3; r++) CHECK_EQ_INT(d->candidate->policy[r], -1);
        CHECK_NEAR(gateRowThreshold(d, 1), 0.81061492126959089, 1e-14);
        CHECK(gateRowThreshold(d, 0) < 0 && gateRowThreshold(d, 2) < 0);
        CHECK(gateState(d) == GATE_STATE_PAUSED);
        gateFree(d);
    }
    blob_free(&got);
    blob_free(&want);
    gateFree(g);
}

/* The digests of the two reference gates are the reference blobs with the
 * primary-only fields zeroed, at the offsets the Python script reports. */
TEST(reference_digests) {
    static const struct {
        const char *text;
        size_t now, counters;
    } ref[] = {
        {golden_score_text, GOLDEN_SCORE_NOW, GOLDEN_SCORE_COUNTERS},
        {golden_classifier_text, GOLDEN_CLASSIFIER_NOW, GOLDEN_CLASSIFIER_COUNTERS},
    };

    for (int i = 0; i < 2; i++) {
        gate *g = i == 0 ? golden_score_gate() : golden_classifier_gate();
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
    gate *ref = golden_classifier_gate();

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
    gateFree(ref);
}

/* runtime_tag belongs to the embedding layer: it is neither encoded nor
 * kept (check_round_trip() checks that a copy starts with 0). */
TEST(runtime_tag_is_not_encoded) {
    gate *g = golden_score_gate();
    blob a = encode(g, 0), b;

    g->runtime_tag = ~0ull;
    b = encode(g, 0);
    CHECK(blob_equal(&a, &b));
    blob_free(&a);
    blob_free(&b);
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
    CHECK(w.unanswered == 1 && w.answered == 2);
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
    /* 8 + config (41 + 64 * (4 + 64) + 64 * 9) + 49 + trainer (1 + 2097416)
     * + candidate (1 + 72 + 2097416 + 64 * 1824) + 1 + 12 + 120 bytes,
     * a model being 8 + 64 * 8192 * 4 + 64 * 4 = 2097416. */
    CHECK_EQ_INT(layout_of(g).end, 4316801);
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
        gateObs *o = gateObsAllocate();

        CHECK_EQ_STR(gate_diff(g, copy), "");
        o->seq = copy->next_seq++;
        o->created_ms = copy->now_ms;
        gateObsLink(copy, o);
        b = encode(copy, 0);
        REJECT(b.p, b.n, "too many waiting requests");
        blob_free(&b);
        gateFree(copy);
    }
    gateFree(g);
}

/* A gate that holds as many requests as it may is still a gate. */
TEST(full_of_requests) {
    gate *g = new_gate(GATE_KIND_SCORE, 0);
    gatePredictResult res;
    gateEval ev;

    for (uint32_t i = 0; i < GATE_MAX_PENDING + 5; i++) {
        MUST(gateEvaluate(g, NULL, 0.5, &ev) == GATE_OK);
        MUST(gatePredict(g, g->now_ms, 0.9, &ev, NULL, &res) == GATE_OK);
    }
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

/* The same for a change that is unusual but legitimate: it must decode and
 * encode back to the same bytes. */
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

TEST(hostile_header) {
    gate *ref = golden_score_gate();
    blob b = encode(ref, 0);
    unsigned char *longer = malloc(b.n + 1);

    PATCHED(ref, 0, 'g', 1, "bad magic");
    PATCHED(ref, 3, 0, 1, "bad magic");
    PATCHED(ref, 4, 2, 2, "unsupported codec version");
    PATCHED(ref, 4, 0, 2, "unsupported codec version");
    PATCHED(ref, 4, 0x0100, 2, "unsupported codec version"); /* 1, big endian */
    PATCHED(ref, 6, 2, 2, "unsupported features version");
    PATCHED(ref, 6, 0, 2, "unsupported features version");

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

TEST(hostile_config) {
    gate *ref = golden_classifier_gate(), *few;
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
    gate *ref = golden_classifier_gate(), *score = golden_score_gate();
    layout l = layout_of(ref), ls = layout_of(score);

    PATCHED(ref, l.ledger + LEDGER_PAUSED, 2, 1, "invalid flag");
    HOSTILE(ref, "clock out of range", g->now_ms = INT64_MAX);
    HOSTILE(ref, "clock out of range",
            g->now_ms = INT64_MAX - GATE_MAX_FEEDBACK_TIMEOUT_MS + 1);
    HOSTILE(ref, "counter out of range", g->generations = 1ull << 63);
    HOSTILE(ref, "counter out of range", g->next_seq = ~0ull);
    HOSTILE(ref, "invalid request counter", g->next_seq = 0);

    PATCHED(ref, l.trainer, 0, 1, "trainer does not match the gate kind");
    PATCHED(ref, l.trainer, 2, 1, "invalid flag");
    PATCHED(score, ls.trainer, 1, 1, "trainer does not match the gate kind");
    HOSTILE(ref, "weight out of range", g->trainer.w[100] = NAN);
    HOSTILE(ref, "weight out of range", g->trainer.w[0] = INFINITY);
    HOSTILE(ref, "weight out of range", g->trainer.w[24575] = 64.00001f);
    HOSTILE(ref, "weight out of range", g->trainer.b[1] = -65.0f);
    HOSTILE(ref, "weight out of range", g->candidate->model.w[7] = 1e30f);
    HOSTILE(ref, "weight out of range", g->serving->model.b[2] = -INFINITY);

    PATCHED(ref, l.candidate, 2, 1, "invalid flag");
    PATCHED(ref, l.serving, 0xff, 1, "invalid flag");
    HOSTILE(ref, "counter out of range", g->candidate->samples = 1ull << 63);
    HOSTILE(ref, "counter out of range", g->serving->audits = ~0ull);
    HOSTILE(ref, "non-finite number", g->candidate->regions[1].logw[2] = NAN);
    HOSTILE(ref, "non-finite number", g->serving->regions[40].logw[0] = INFINITY);
    HOSTILE(ref, "non-finite number", g->serving->drift[1].logw[1] = -INFINITY);
    HOSTILE(score, "non-finite number", g->serving->regions[31].logw[3] = -NAN);

    /* What gateCheckInvariants() says about generations. */
    HOSTILE(ref, "candidate: generation number out of range", g->candidate->gen = 0);
    HOSTILE(ref, "candidate: generation number out of range", g->candidate->gen = 8);
    HOSTILE(ref, "serving: generation number out of range", g->serving->gen = 9);
    HOSTILE(ref, "candidate and serving share a number", g->candidate->gen = 5);
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
    /* Nothing crossed at an entry point any more: the policy is computed
     * from the regions, and a generation without one does not serve. */
    HOSTILE(ref, "serving has no proven threshold", g->serving->regions[32 + 7].certified_at = 0);
    HOSTILE(score, "serving has no proven threshold", g->serving->regions[0].certified_at = 0);

    gateFree(ref);
    gateFree(score);
}

TEST(hostile_requests) {
    gate *ref = golden_classifier_gate(), *score = golden_score_gate();
    layout l = layout_of(ref), ls = layout_of(score);
    size_t at = request_at(ref, &l, 1) + REQUEST_NCLUES; /* request 3 has three clues */
    blob b;

    HOSTILE(ref, "requests out of order", gateFindObservation(g, 2)->seq = 0);
    HOSTILE(ref, "requests out of order", gateFindObservation(g, 2)->seq = 3);
    HOSTILE(ref, "requests out of order", gateFindObservation(g, 2)->seq = 5);
    HOSTILE(ref, "requests out of order", g->next_seq = 9);
    HOSTILE(ref, "request from the future", g->newest->created_ms++);

    HOSTILE(ref, "invalid request flags", g->oldest->flags = 1u << 6);
    HOSTILE(ref, "invalid request flags", g->oldest->flags = 1u << 31);
    HOSTILE(ref, "invalid request flags", g->oldest->flags = GATE_OBS_MISSING);
    HOSTILE(ref, "answered request outside the evidence queue",
            g->oldest->flags = GATE_OBS_RESOLVED; g->oldest->result = 1);
    HOSTILE(ref, "answered live evidence left at the head",
            gateFindObservation(g, 3)->flags |= GATE_OBS_RESOLVED;
            gateFindObservation(g, 3)->result = 1);

    HOSTILE(ref, "invalid request answer", gateFindObservation(g, 4)->result = 3);
    HOSTILE(ref, "invalid request answer", gateFindObservation(g, 4)->result = -1);
    HOSTILE(ref, "invalid request answer", gateFindObservation(g, 3)->result = 0);
    HOSTILE(ref, "invalid request answer", gateFindObservation(g, 3)->result = -2);
    HOSTILE(score, "invalid request answer", g->newest->result = 2);

    HOSTILE(ref, "invalid request guess", gateFindObservation(g, 3)->cand_label = 3);
    HOSTILE(ref, "invalid request guess", gateFindObservation(g, 3)->cand_label = -1);
    HOSTILE(ref, "invalid request guess", gateFindObservation(g, 3)->serv_label = -2);
    HOSTILE(ref, "invalid request guess", gateFindObservation(g, 4)->serv_label = INT32_MAX);
    HOSTILE(ref, "invalid request guess", g->oldest->cand_label = 0);
    HOSTILE(ref, "invalid request guess", g->oldest->serv_gen = 5);
    HOSTILE(ref, "invalid request guess", gateFindObservation(g, 3)->cand_gen = 8);
    HOSTILE(ref, "invalid request guess", g->generations = 6);
    HOSTILE(score, "invalid request guess", g->oldest->serv_label = 0);
    HOSTILE(score, "invalid request guess", g->oldest->cand_label = 0);

    HOSTILE(ref, "non-finite number", gateFindObservation(g, 3)->cand_score = NAN);
    HOSTILE(ref, "non-finite number", gateFindObservation(g, 3)->serv_score = INFINITY);
    HOSTILE(score, "non-finite number", g->oldest->serv_score = -INFINITY);

    HOSTILE(ref, "invalid clues", gateFindObservation(g, 3)->x.idx[2] = GATE_DIM);
    HOSTILE(ref, "invalid clues", gateFindObservation(g, 3)->x.idx[1] = 5);
    HOSTILE(ref, "invalid clues", gateFindObservation(g, 3)->x.idx[1] = 4);
    HOSTILE(ref, "invalid clues", gateFindObservation(g, 3)->x.idx[0] = 0xffffffffu);
    HOSTILE(ref, "invalid clues", gateFindObservation(g, 3)->x.val[0] = 0.0f);
    HOSTILE(ref, "invalid clues", gateFindObservation(g, 3)->x.val[1] = -0.0f);
    HOSTILE(ref, "invalid clues", gateFindObservation(g, 3)->x.val[2] = NAN);
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
    put(b.p + ls.requests + 8, 3, 4);
    REJECT_ANY(b.p, b.n);
    put(b.p + ls.requests + 8, 1, 4);
    REJECT_ANY(b.p, b.n);
    put(b.p + ls.requests + 8, 0, 4);
    REJECT_ANY(b.p, b.n);
    blob_free(&b);

    gateFree(ref);
    gateFree(score);
}

TEST(hostile_counters) {
    gate *ref = golden_classifier_gate();

    HOSTILE(ref, "request counters do not add up", g->stats.requests++);
    HOSTILE(ref, "request counters do not add up", g->stats.abstained--);
    HOSTILE(ref, "coverage out of range", g->stats.window = GATE_COVERAGE_WINDOW + 1);
    HOSTILE(ref, "coverage out of range", g->stats.coverage = 1.0000001);
    HOSTILE(ref, "coverage out of range", g->stats.coverage = -0.25);
    HOSTILE(ref, "non-finite number", g->stats.coverage = NAN);
    gateFree(ref);
}

/* Extreme values that a gate may hold. */
TEST(unusual_but_valid) {
    gate *ref = golden_classifier_gate(), *score = golden_score_gate();

    ACCEPTED(ref, g->now_ms = INT64_MAX - GATE_MAX_FEEDBACK_TIMEOUT_MS);
    ACCEPTED(ref, set_all_times(g, INT64_MIN));
    ACCEPTED(ref, g->lineage = g->epoch = ~0ull);
    ACCEPTED(ref, g->generations = INT64_MAX; g->next_seq = INT64_MAX);
    ACCEPTED(ref, g->candidate->samples = g->serving->audits = INT64_MAX);
    /* Counters nothing is compared with may hold anything. */
    ACCEPTED(ref, g->candidate->budget = ~0ull; g->candidate->paired = ~0ull);
    ACCEPTED(ref, g->trainer.updates = ~0ull; g->snapshot_updates = 5);
    ACCEPTED(ref, g->trainer.updates = 0; g->snapshot_updates = ~0ull);
    ACCEPTED(ref, g->serving->drift[1].n = 0; g->serving->drift[1].errors = ~0ull);
    ACCEPTED(ref, g->stats.learned = g->stats.ignored = g->stats.missing = ~0ull);
    /* The request counters add up modulo 2^64, as they would after wrapping. */
    ACCEPTED(ref, g->stats.requests = 5; g->stats.accepted = ~0ull; g->stats.audited = 3;
             g->stats.abstained = 3);
    ACCEPTED(ref, g->stats.window = GATE_COVERAGE_WINDOW; g->stats.coverage = 1.0);
    ACCEPTED(ref, g->stats.window = 0; g->stats.coverage = -0.0);

    ACCEPTED(ref, g->cfg.audit = 1.0; g->cfg.delta = 0.999; g->cfg.snapshot_examples = UINT32_MAX);
    ACCEPTED(ref, g->cfg.feedback_timeout_ms = GATE_MAX_FEEDBACK_TIMEOUT_MS);
    ACCEPTED(ref, g->cfg.alpha = 1e-300; g->cfg.class_alpha[0] = g->cfg.class_alpha[1] =
             g->cfg.class_alpha[2] = 5e-324);
    ACCEPTED(ref, g->trainer.w[0] = -64.0f; g->trainer.w[1] = -0.0f; g->trainer.b[0] = FLT_MIN);
    ACCEPTED(ref, g->candidate->regions[0].logw[0] = -DBL_MAX;
             g->serving->drift[0].logw[1] = DBL_MAX);
    ACCEPTED(ref, gateFindObservation(g, 3)->cand_score = -0.0;
             gateFindObservation(g, 3)->serv_score = 1e300);
    ACCEPTED(ref, gateFindObservation(g, 3)->x.val[0] = FLT_MAX;
             gateFindObservation(g, 3)->x.val[1] = -FLT_MIN;
             gateFindObservation(g, 3)->x.idx[0] = 0);
    ACCEPTED(score, g->oldest->serv_score = -1.5; g->newest->result = 0);
    /* A request that is evidence for a generation that is gone. */
    ACCEPTED(score, g->oldest->serv_gen = 2; g->newest->serv_gen = 1);

    gateFree(ref);
    gateFree(score);
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

TEST(truncated_blobs) {
    check_truncations(golden_score_gate(), 1);
    check_truncations(score_with_requests(31, 0), 1);
    check_truncations(new_gate(GATE_KIND_CLASSIFIER, 2), 1);
    check_truncations(classifier_learning(32), 1);
    check_truncations(classifier_with_everything(33, 2), 509);
    check_truncations(golden_classifier_gate(), 8191);
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
    CHECK(n >= 5 + (long)g->npending);
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
    check_allocation_failures(golden_score_gate());
    check_allocation_failures(classifier_with_everything(41, 2));
    check_allocation_failures(golden_classifier_gate());
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

/* ---------------------------- Random corruption -------------------------- */

static long fuzz_count = 200000;
static long fuzz_accepted = 0, fuzz_rejected = 0;

/* Whatever the decoder lets through is a gate, and must be safe to use:
 * answer what waits (newest first, so that answers pile up behind the
 * oldest), ask some more, let the rest time out. It must still be
 * consistent then and, if 'again' is set, encode to something that
 * decodes. */
static void exercise(gate *g, uint64_t seed, int again) {
    const int64_t last = INT64_MAX - GATE_MAX_FEEDBACK_TIMEOUT_MS;
    uint64_t answers = g->cfg.kind == GATE_KIND_CLASSIFIER ? g->cfg.nclasses : 2;
    uint64_t ids[32], limit = 1ull << 62;
    sim s = {seed, g->now_ms, 0};
    char err[160] = "";
    int n = 0;
    blob b;
    gate *copy;

    for (const gateObs *o = g->oldest; o && n < 32; o = o->next)
        if (!(o->flags & GATE_OBS_RESOLVED)) ids[n++] = o->seq;
    while (n > 0) {
        n--;
        gateLearn(g, s.now, g->epoch, ids[n], (int32_t)(ids[n] % answers));
    }
    /* Unless the clock of the blob leaves no room to move. */
    if (g->now_ms < last - 1000000) {
        teach(&s, g, 4);
        sim_ask(&s, g);
        sim_ask(&s, g);
    }
    gateMaintain(g, last);
    MUST(gateCheckInvariants(g, err, sizeof(err)));
    if (!again) return;

    /* The four counters the decoder bounds may have been pushed over the
     * bound by a blob that put them right below it. */
    if (g->next_seq >= limit || g->generations >= limit) return;
    if (g->candidate && (g->candidate->samples >= limit || g->candidate->audits >= limit)) return;
    if (g->serving && (g->serving->samples >= limit || g->serving->audits >= limit)) return;
    b = encode(g, 0);
    copy = gateDecode(b.p, b.n, err, sizeof(err));
    MUST(copy != NULL);
    gateFree(copy);
    blob_free(&b);
}

/* Field-sized runs that numbers tend to be wrong in: extremes, infinities
 * and NaN as a double and as two floats, a weight just over the limit, one
 * bucket too many. */
static const uint64_t fuzz_runs[] = {
    0, ~0ull, 1, 0x7fffffffffffffffull, 0x8000000000000000ull, 0xffffffff00000000ull,
    0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000000ull,
    0x7f8000007f800000ull, 0x7fc000007fc00000ull, 0x4280000142800001ull,
    0x0000200000002000ull,
};

/* 'count' corruptions of the encoding of 'g': one byte, one bit, a few
 * bytes anywhere, a run from the list above, or a small number. Each must
 * be refused with a reason, or give a gate that encodes back to exactly
 * the corrupted bytes and can be used. Releases 'g'. */
static void corrupt(gate *g, long count, uint64_t seed) {
    blob b = encode(g, 0);
    layout l = layout_of(g);
    unsigned char *buf = malloc(b.n); /* exactly as long: overreads show */
    uint64_t rng = seed;

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
            if (kind >= 6) byte = (unsigned char)(run >> (8 * j));
            else if (kind >= 3) byte = buf[pos] ^ (unsigned char)(1 + gate_test_rand64(&rng) % 255);
            else byte = buf[pos] ^ (unsigned char)(1u << (r >> 8) % 8);
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

TEST(random_corruptions) {
    corrupt(golden_score_gate(), fuzz_count * 30 / 100, 51);
    corrupt(score_with_requests(52, 0), fuzz_count * 30 / 100, 52);
    corrupt(score_with_requests(53, 1), fuzz_count * 25 / 100, 53);
    corrupt(classifier_learning(54), fuzz_count * 10 / 100, 54);
    corrupt(classifier_with_everything(55, 2), fuzz_count * 5 / 100, 55);
    printf("  %ld corruptions: %ld refused, %ld decoded\n", fuzz_accepted + fuzz_rejected,
           fuzz_rejected, fuzz_accepted);
    CHECK(fuzz_accepted + fuzz_rejected >= fuzz_count * 99 / 100);
    /* Both happen a lot: a corruption that goes unnoticed (a counter, a
     * weight) and one that is caught. */
    CHECK(fuzz_accepted * 10 > fuzz_count && fuzz_rejected * 10 > fuzz_count);
}

int main(int argc, char **argv) {
    if (argc > 1) fuzz_count = strtol(argv[1], NULL, 10);

    RUN(reference_score_gate);
    RUN(reference_classifier_gate);
    RUN(reference_digests);
    RUN(digest_fields);
    RUN(runtime_tag_is_not_encoded);

    RUN(score_fresh);
    RUN(score_calibrating);
    RUN(score_active);
    RUN(score_paused);
    RUN(score_after_drift);
    RUN(score_after_reset);
    RUN(score_after_recover);
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

    RUN(widest_classifier);
    RUN(full_of_requests);
    RUN(full_of_clues);

    RUN(hostile_header);
    RUN(hostile_config);
    RUN(hostile_ledger_and_models);
    RUN(hostile_requests);
    RUN(hostile_counters);
    RUN(unusual_but_valid);
    RUN(error_buffer);
    RUN(truncated_blobs);
    RUN(allocation_failures);
    RUN(no_allocation_on_credit);
    RUN(random_corruptions);
    return TEST_REPORT();
}
