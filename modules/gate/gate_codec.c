/*
 * Gate data type: binary encoding of a whole gate. See gate_codec.h for the
 * format rules and for what the decoder promises about untrusted input.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * LAYOUT, version 1. Integers are little endian, f64 and f32 are IEEE-754
 * bit patterns. K is the number of classes (0 for a score gate), V the
 * number of weight vectors (1 when K is 2, else K) and R the number of rows
 * (K, or 1 for a score gate).
 *
 *   header      "GATE", u16 codec version, u16 features version
 *   config      u8 kind, f64 alpha, f64 delta, f64 audit, u32 K,
 *               K x (u32 length, label bytes), K x f64 class alpha,
 *               K x u8 class defer, u32 snapshot examples,
 *               i64 feedback timeout
 *   ledger      u64 lineage, u64 epoch, i64 created_ms, i64 now_ms *,
 *               u64 generations, u8 paused, u64 snapshot_updates
 *   trainer     u8 present (1 for a classifier, else 0), then a model
 *   candidate   u8 present, then a generation
 *   serving     u8 present, then a generation
 *   requests    u64 next_seq, u32 count, then the waiting requests in
 *               ascending seq order
 *   counters    u64 requests *, accepted *, audited *, abstained *, full *,
 *               window *, f64 coverage *, u64 observations, learned,
 *               ignored, missing, dropped, promotions, revocations, retired
 *
 *   model       u64 updates, V * GATE_DIM x f32 weights, V x f32 biases
 *   generation  u64 gen, i64 frozen_ms, i64 promoted_ms, u64 samples,
 *               errors, paired, audits, audit_errors, budget, a model
 *               (classifier only), R * GATE_THRESHOLDS x region (4 x f64
 *               log wealth, u64 n, errors, certified_at), R x drift
 *               detector (2 x f64 log statistic, u64 n, errors)
 *   request     u64 seq, i64 created_ms, u32 flags, i32 cand_label,
 *               i32 serv_label, f64 cand_score, f64 serv_score,
 *               u64 cand_gen, u64 serv_gen, i32 result, u32 n,
 *               n x u32 bucket, n x f32 value
 *
 * The fields marked * are written as zero for a digest. The bar and the
 * policy of a generation are not stored: the decoder computes them again.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "gate_codec.h"
#include "gate_features.h" /* GATE_FEATURES_VERSION */

/* The decoder rejects NaN and infinities with isfinite() and with
 * comparisons that a NaN fails. Under -ffast-math the compiler may assume
 * such values do not exist and delete the checks. */
#if defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__
#error "gate_codec.c needs IEEE NaN and infinity semantics: do not build it with -ffast-math"
#endif

/* The low 'n' bytes of 'v', least significant first, and back. */
static void gatePut(unsigned char *p, uint64_t v, size_t n) {
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)(v >> (8 * i));
}

static uint64_t gateGet(const unsigned char *p, size_t n) {
    uint64_t v = 0;

    for (size_t i = 0; i < n; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

/* --------------------------------- Writer -------------------------------- */

typedef struct gateWriter {
    unsigned char *buf;
    size_t len, cap;
    int failed; /* an allocation failed: sticky, later writes do nothing */
} gateWriter;

/* Make room for 'n' more bytes and return where they go, or NULL. */
static unsigned char *gateWriteRoom(gateWriter *w, size_t n) {
    if (w->failed) return NULL;
    if (n > w->cap - w->len) {
        size_t cap = w->cap ? w->cap : 1024;
        unsigned char *buf = NULL;

        while (cap - w->len < n && cap <= SIZE_MAX / 2) cap *= 2;
        if (cap - w->len >= n) buf = gate_realloc(w->buf, cap);
        if (buf == NULL) {
            w->failed = 1;
            return NULL;
        }
        w->buf = buf;
        w->cap = cap;
    }
    w->len += n;
    return w->buf + w->len - n;
}

static void gateWriteBytes(gateWriter *w, const void *src, size_t n) {
    unsigned char *p = gateWriteRoom(w, n);

    if (p) memcpy(p, src, n);
}

static void gateWriteInt(gateWriter *w, uint64_t v, size_t n) {
    unsigned char *p = gateWriteRoom(w, n);

    if (p) gatePut(p, v, n);
}

static void gateWriteU8(gateWriter *w, uint8_t v) { gateWriteInt(w, v, 1); }
static void gateWriteU32(gateWriter *w, uint32_t v) { gateWriteInt(w, v, 4); }
static void gateWriteU64(gateWriter *w, uint64_t v) { gateWriteInt(w, v, 8); }
static void gateWriteI64(gateWriter *w, int64_t v) { gateWriteInt(w, (uint64_t)v, 8); }

static void gateWriteDouble(gateWriter *w, double v) {
    uint64_t bits;

    memcpy(&bits, &v, sizeof(bits));
    gateWriteInt(w, bits, 8);
}

static void gateWriteFloats(gateWriter *w, const float *v, size_t n) {
    unsigned char *p = gateWriteRoom(w, n * 4);

    if (p == NULL) return;
    for (size_t i = 0; i < n; i++, p += 4) {
        uint32_t bits;

        memcpy(&bits, &v[i], sizeof(bits));
        gatePut(p, bits, 4);
    }
}

/* --------------------------------- Encoder ------------------------------- */

static void gateWriteModel(gateWriter *w, const gateModel *m) {
    gateWriteU64(w, m->updates);
    gateWriteFloats(w, m->w, (size_t)m->nvec * m->dim);
    gateWriteFloats(w, m->b, m->nvec);
}

static void gateWriteGeneration(gateWriter *w, const gate *g, const gateGeneration *gen) {
    gateWriteU8(w, gen != NULL);
    if (gen == NULL) return;

    gateWriteU64(w, gen->gen);
    gateWriteI64(w, gen->frozen_ms);
    gateWriteI64(w, gen->promoted_ms);
    gateWriteU64(w, gen->samples);
    gateWriteU64(w, gen->errors);
    gateWriteU64(w, gen->paired);
    gateWriteU64(w, gen->audits);
    gateWriteU64(w, gen->audit_errors);
    gateWriteU64(w, gen->budget);
    if (g->cfg.kind == GATE_KIND_CLASSIFIER) gateWriteModel(w, &gen->model);
    for (uint32_t i = 0; i < g->nrows * GATE_THRESHOLDS; i++) {
        const gateRegion *reg = &gen->regions[i];

        for (int j = 0; j < GATE_NBETS; j++) gateWriteDouble(w, reg->logw[j]);
        gateWriteU64(w, reg->n);
        gateWriteU64(w, reg->errors);
        gateWriteU64(w, reg->certified_at);
    }
    for (uint32_t row = 0; row < g->nrows; row++) {
        const gateDrift *d = &gen->drift[row];

        for (int k = 0; k < GATE_DRIFT_ALTS; k++) gateWriteDouble(w, d->logw[k]);
        gateWriteU64(w, d->n);
        gateWriteU64(w, d->errors);
    }
}

static void gateWriteObs(gateWriter *w, const gateObs *o) {
    gateWriteU64(w, o->seq);
    gateWriteI64(w, o->created_ms);
    gateWriteU32(w, o->flags);
    gateWriteU32(w, (uint32_t)o->cand_label);
    gateWriteU32(w, (uint32_t)o->serv_label);
    gateWriteDouble(w, o->cand_score);
    gateWriteDouble(w, o->serv_score);
    gateWriteU64(w, o->cand_gen);
    gateWriteU64(w, o->serv_gen);
    gateWriteU32(w, (uint32_t)o->result);
    gateWriteU32(w, o->x.n);
    for (uint32_t i = 0; i < o->x.n; i++) gateWriteU32(w, o->x.idx[i]);
    gateWriteFloats(w, o->x.val, o->x.n);
}

int gateEncode(const gate *g, int for_digest, unsigned char **buf, size_t *len) {
    gateWriter w = {NULL, 0, 0, 0};
    const gateConfig *cfg = &g->cfg;
    int classifier = cfg->kind == GATE_KIND_CLASSIFIER;
    int64_t now_ms = g->now_ms;
    gateCounters s = g->stats;

    if (for_digest) {
        /* What only the primary maintains: a replica never sees an ACCEPT. */
        now_ms = 0;
        s.requests = s.accepted = s.audited = s.abstained = s.full = s.window = 0;
        s.coverage = 0.0;
    }

    gateWriteBytes(&w, "GATE", 4);
    gateWriteInt(&w, GATE_CODEC_VERSION, 2);
    gateWriteInt(&w, GATE_FEATURES_VERSION, 2);

    gateWriteU8(&w, (uint8_t)cfg->kind);
    gateWriteDouble(&w, cfg->alpha);
    gateWriteDouble(&w, cfg->delta);
    gateWriteDouble(&w, cfg->audit);
    gateWriteU32(&w, cfg->nclasses);
    for (uint32_t i = 0; i < cfg->nclasses; i++) {
        size_t n = strlen(cfg->labels[i]);

        gateWriteU32(&w, (uint32_t)n);
        gateWriteBytes(&w, cfg->labels[i], n);
    }
    for (uint32_t i = 0; i < cfg->nclasses; i++) gateWriteDouble(&w, cfg->class_alpha[i]);
    for (uint32_t i = 0; i < cfg->nclasses; i++) gateWriteU8(&w, cfg->class_defer[i]);
    gateWriteU32(&w, cfg->snapshot_examples);
    gateWriteI64(&w, cfg->feedback_timeout_ms);

    gateWriteU64(&w, g->lineage);
    gateWriteU64(&w, g->epoch);
    gateWriteI64(&w, g->created_ms);
    gateWriteI64(&w, now_ms);
    gateWriteU64(&w, g->generations);
    gateWriteU8(&w, g->paused != 0);
    gateWriteU64(&w, g->snapshot_updates);

    gateWriteU8(&w, (uint8_t)classifier);
    if (classifier) gateWriteModel(&w, &g->trainer);
    gateWriteGeneration(&w, g, g->candidate);
    gateWriteGeneration(&w, g, g->serving);

    gateWriteU64(&w, g->next_seq);
    gateWriteU32(&w, g->npending);
    for (const gateObs *o = g->oldest; o; o = o->next) gateWriteObs(&w, o);

    gateWriteU64(&w, s.requests);
    gateWriteU64(&w, s.accepted);
    gateWriteU64(&w, s.audited);
    gateWriteU64(&w, s.abstained);
    gateWriteU64(&w, s.full);
    gateWriteU64(&w, s.window);
    gateWriteDouble(&w, s.coverage);
    gateWriteU64(&w, s.observations);
    gateWriteU64(&w, s.learned);
    gateWriteU64(&w, s.ignored);
    gateWriteU64(&w, s.missing);
    gateWriteU64(&w, s.dropped);
    gateWriteU64(&w, s.promotions);
    gateWriteU64(&w, s.revocations);
    gateWriteU64(&w, s.retired);

    if (w.failed) {
        gate_free(w.buf);
        *buf = NULL;
        *len = 0;
        return GATE_ERR;
    }
    *buf = w.buf;
    *len = w.len;
    return GATE_OK;
}

/* --------------------------------- Reader --------------------------------
 * The input is untrusted. Every read checks what is left, and the first
 * failure is sticky: from then on reads return zero and consume nothing, so
 * the decoding functions below read their fields in a row and need not test
 * each one. What they must never do is use a value to size a loop or an
 * allocation before checking it against a limit. */

typedef struct gateReader {
    const unsigned char *p; /* next unread byte */
    size_t len;             /* bytes left */
    int failed;
    char *err;              /* receives the reason of the first failure */
    size_t errlen;
} gateReader;

static void gateReadFail(gateReader *r, const char *reason) {
    if (r->failed) return;
    r->failed = 1;
    if (r->err && r->errlen) snprintf(r->err, r->errlen, "%s", reason);
}

/* Consume 'n' bytes and return them, or NULL if fewer are left. */
static const unsigned char *gateReadBytes(gateReader *r, size_t n) {
    const unsigned char *p = r->p;

    if (n > r->len) gateReadFail(r, "truncated");
    if (r->failed) return NULL;
    r->p += n;
    r->len -= n;
    return p;
}

static uint64_t gateReadInt(gateReader *r, size_t n) {
    const unsigned char *p = gateReadBytes(r, n);

    return p ? gateGet(p, n) : 0;
}

static uint8_t gateReadU8(gateReader *r) { return (uint8_t)gateReadInt(r, 1); }
static uint32_t gateReadU32(gateReader *r) { return (uint32_t)gateReadInt(r, 4); }
static uint64_t gateReadU64(gateReader *r) { return gateReadInt(r, 8); }
static int64_t gateReadI64(gateReader *r) { return (int64_t)gateReadInt(r, 8); }

/* A counter the core keeps in order with others: the numbers of requests
 * and generations ascend, a part stays below its total. That holds while
 * it does not wrap around, so it may not start near the end of its range.
 * Counting never gets it anywhere near 2^63. */
static uint64_t gateReadCount(gateReader *r) {
    uint64_t v = gateReadInt(r, 8);

    if (v > INT64_MAX) gateReadFail(r, "counter out of range");
    return v;
}

static int gateReadBool(gateReader *r) {
    uint8_t v = gateReadU8(r);

    if (v > 1) gateReadFail(r, "invalid flag");
    return v == 1;
}

/* No field of a gate is ever NaN or infinite, so none is let in. */
static double gateReadDouble(gateReader *r) {
    uint64_t bits = gateReadInt(r, 8);
    double v;

    memcpy(&v, &bits, sizeof(v));
    if (isfinite(v)) return v;
    gateReadFail(r, "non-finite number");
    return 0.0;
}

static float gateGetFloat(const unsigned char *p) {
    uint32_t bits = (uint32_t)gateGet(p, 4);
    float v;

    memcpy(&v, &bits, sizeof(v));
    return v;
}

/* --------------------------------- Decoder ------------------------------- */

/* Fill an allocated model. The model saturates its weights, so anything
 * beyond the limit (NaN and infinities fail the comparison too) was never
 * written by it. */
static void gateReadModel(gateReader *r, gateModel *m) {
    size_t nw = (size_t)m->nvec * m->dim, n = nw + m->nvec;
    const unsigned char *p;

    m->updates = gateReadU64(r);
    p = gateReadBytes(r, n * 4);
    if (p == NULL) return;
    for (size_t i = 0; i < n; i++, p += 4) {
        float v = gateGetFloat(p);

        if (!(fabsf(v) <= GATE_MAX_WEIGHT)) {
            gateReadFail(r, "weight out of range");
            return;
        }
        if (i < nw) m->w[i] = v; else m->b[i - nw] = v;
    }
}

/* A config the caller releases with gateConfigFree(), whatever happens. */
static void gateReadConfig(gateReader *r, gateConfig *cfg) {
    const char *labels[GATE_MAX_CLASSES];
    size_t lens[GATE_MAX_CLASSES];
    uint32_t n;

    gateConfigInit(cfg);
    cfg->kind = gateReadU8(r);
    cfg->alpha = gateReadDouble(r);
    cfg->delta = gateReadDouble(r);
    cfg->audit = gateReadDouble(r);
    n = gateReadU32(r);
    if (n > GATE_MAX_CLASSES) {
        gateReadFail(r, "too many classes");
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        lens[i] = gateReadU32(r);
        labels[i] = (const char *)gateReadBytes(r, lens[i]);
        /* Checked here with its length: the config keeps C strings, so a
         * label with a zero byte would come back shorter than it went in. */
        if (labels[i] && !gateValidName(labels[i], lens[i], GATE_MAX_LABEL_BYTES))
            gateReadFail(r, "invalid label");
    }
    if (r->failed) return;
    if (n > 0 && gateConfigSetClasses(cfg, labels, lens, n) != GATE_OK) {
        gateReadFail(r, "out of memory");
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        cfg->class_alpha[i] = gateReadDouble(r);
        /* gateConfigValidate() takes 0 for "not set" and fills it in: the
         * gate would not encode back to the bytes it came from. */
        if (cfg->class_alpha[i] == 0.0) gateReadFail(r, "invalid class alpha");
    }
    for (uint32_t i = 0; i < n; i++) cfg->class_defer[i] = (uint8_t)gateReadBool(r);
    cfg->snapshot_examples = gateReadU32(r);
    cfg->feedback_timeout_ms = gateReadI64(r);
}

/* The candidate or the serving generation: NULL if the blob has none, or
 * after a failure before it could be allocated. */
static gateGeneration *gateReadGeneration(gateReader *r, const gate *g) {
    int classifier = g->cfg.kind == GATE_KIND_CLASSIFIER;
    gateGeneration *gen;

    if (!gateReadBool(r)) return NULL;
    /* The frozen model is the one large allocation: the blob must be long
     * enough to hold it before it is made. */
    if (r->len < gateModelBytes(GATE_DIM, g->cfg.nclasses)) {
        gateReadFail(r, "truncated");
        return NULL;
    }
    gen = gateGenerationAllocate(g);
    if (gen == NULL ||
        (classifier && gateModelInit(&gen->model, GATE_DIM, g->cfg.nclasses) != GATE_OK))
    {
        gateGenerationRelease(gen);
        gateReadFail(r, "out of memory");
        return NULL;
    }

    gen->gen = gateReadU64(r);
    gen->frozen_ms = gateReadI64(r);
    gen->promoted_ms = gateReadI64(r);
    gen->samples = gateReadCount(r);
    gen->errors = gateReadU64(r);
    gen->paired = gateReadU64(r);
    gen->audits = gateReadCount(r);
    gen->audit_errors = gateReadU64(r);
    gen->budget = gateReadU64(r);
    if (classifier) gateReadModel(r, &gen->model);
    for (uint32_t i = 0; i < g->nrows * GATE_THRESHOLDS; i++) {
        gateRegion *reg = &gen->regions[i];

        for (int j = 0; j < GATE_NBETS; j++) reg->logw[j] = gateReadDouble(r);
        reg->n = gateReadU64(r);
        reg->errors = gateReadU64(r);
        reg->certified_at = gateReadU64(r);
    }
    for (uint32_t row = 0; row < g->nrows; row++) {
        gateDrift *d = &gen->drift[row];

        for (int k = 0; k < GATE_DRIFT_ALTS; k++) d->logw[k] = gateReadDouble(r);
        d->n = gateReadU64(r);
        d->errors = gateReadU64(r);
        gateGenerationRefreshPolicy(g, gen, row);
    }
    gen->log_threshold = gateBudgetLogThreshold(g->cfg.delta, gen->gen,
                                                gateTestedRows(g) * gateEntryPoints(g));
    return gen;
}

static void gateReadClues(gateReader *r, gateSparse *x) {
    uint32_t n = gateReadU32(r);
    const unsigned char *p;

    if (n > GATE_DIM) {
        gateReadFail(r, "too many clues");
        return;
    }
    p = gateReadBytes(r, (size_t)n * 8); /* they are all there: now allocate */
    if (p == NULL || n == 0) return;
    x->idx = gate_malloc(sizeof(uint32_t) * n);
    x->val = gate_malloc(sizeof(float) * n);
    if (x->idx == NULL || x->val == NULL) {
        gateReadFail(r, "out of memory");
        return;
    }
    x->n = n;
    for (uint32_t i = 0; i < n; i++) {
        x->idx[i] = (uint32_t)gateGet(p + (size_t)i * 4, 4);
        x->val[i] = gateGetFloat(p + ((size_t)n + i) * 4);
        /* What the model relies on: see gateSparse in gate_defs.h. */
        if (x->idx[i] >= GATE_DIM || (i > 0 && x->idx[i] <= x->idx[i - 1]) ||
            !isfinite(x->val[i]) || x->val[i] == 0.0f)
        {
            gateReadFail(r, "invalid clues");
            return;
        }
    }
}

/* The flags a request can rest with. GATE_OBS_MISSING is not one of them:
 * a request that timed out is applied before the gate comes to rest. */
#define GATE_OBS_STORED \
    (GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_EVIDENCE | GATE_OBS_RESOLVED)

/* A frozen guess as gatePredict() records it: no label for a score gate or
 * when the generation did not exist, else one of the K. The core indexes
 * rows with it. */
static int gateGuessValid(const gate *g, uint64_t gen, int32_t label) {
    if (gen > g->generations) return 0;
    if (gen == 0 || g->cfg.kind == GATE_KIND_SCORE) return label == -1;
    return label >= 0 && (uint32_t)label < g->cfg.nclasses;
}

static void gateReadObs(gateReader *r, const gate *g, gateObs *o) {
    /* An answer is a label, or wrong / right for a score gate. */
    int32_t answers = g->cfg.kind == GATE_KIND_CLASSIFIER ? (int32_t)g->cfg.nclasses : 2;
    int answered;

    o->seq = gateReadU64(r);
    o->created_ms = gateReadI64(r);
    o->flags = gateReadU32(r);
    o->cand_label = (int32_t)gateReadU32(r);
    o->serv_label = (int32_t)gateReadU32(r);
    o->cand_score = gateReadDouble(r);
    o->serv_score = gateReadDouble(r);
    o->cand_gen = gateReadU64(r);
    o->serv_gen = gateReadU64(r);
    o->result = (int32_t)gateReadU32(r);
    gateReadClues(r, &o->x);

    if (o->created_ms > g->now_ms) gateReadFail(r, "request from the future");
    if (o->flags & ~(uint32_t)GATE_OBS_STORED) gateReadFail(r, "invalid request flags");
    if (!gateGuessValid(g, o->cand_gen, o->cand_label) ||
        !gateGuessValid(g, o->serv_gen, o->serv_label))
        gateReadFail(r, "invalid request guess");
    answered = (o->flags & GATE_OBS_RESOLVED) != 0;
    if (answered ? (o->result < 0 || o->result >= answers) : o->result != -1)
        gateReadFail(r, "invalid request answer");
}

static void gateReadPending(gateReader *r, gate *g) {
    uint32_t count = gateReadU32(r);
    uint64_t last = 0;

    for (uint32_t i = 0; i < count && !r->failed; i++) {
        gateObs *o;

        /* The core records a request only while the gate is not full, so
         * no gate holds more than this, and no blob makes us allocate
         * more. */
        if (gateFull(g)) {
            gateReadFail(r, "too many waiting requests");
            return;
        }
        o = gateObsAllocate();
        if (o == NULL) {
            gateReadFail(r, "out of memory");
            return;
        }
        gateReadObs(r, g, o);
        if (o->seq <= last || o->seq >= g->next_seq) gateReadFail(r, "requests out of order");
        if (r->failed) {
            gateObsRelease(o);
            return;
        }
        last = o->seq;
        gateObsLink(g, o);
    }
}

/* Everything after the config, into a gate fresh from gateAllocate(). */
static void gateReadState(gateReader *r, gate *g) {
    int classifier = g->cfg.kind == GATE_KIND_CLASSIFIER;
    gateCounters *s = &g->stats;

    g->lineage = gateReadU64(r);
    g->epoch = gateReadU64(r);
    g->created_ms = gateReadI64(r);
    g->now_ms = gateReadI64(r);
    g->generations = gateReadCount(r);
    g->paused = gateReadBool(r);
    g->snapshot_updates = gateReadU64(r);
    /* The core adds the feedback timeout to the time of a request, which
     * is never after the clock (checked for each request). */
    if (g->now_ms > INT64_MAX - GATE_MAX_FEEDBACK_TIMEOUT_MS)
        gateReadFail(r, "clock out of range");

    if (gateReadBool(r) != classifier) gateReadFail(r, "trainer does not match the gate kind");
    if (classifier) gateReadModel(r, &g->trainer);
    g->candidate = gateReadGeneration(r, g);
    g->serving = gateReadGeneration(r, g);

    g->next_seq = gateReadCount(r);
    if (g->next_seq == 0) gateReadFail(r, "invalid request counter");
    gateReadPending(r, g);

    s->requests = gateReadU64(r);
    s->accepted = gateReadU64(r);
    s->audited = gateReadU64(r);
    s->abstained = gateReadU64(r);
    s->full = gateReadU64(r);
    s->window = gateReadU64(r);
    s->coverage = gateReadDouble(r);
    s->observations = gateReadU64(r);
    s->learned = gateReadU64(r);
    s->ignored = gateReadU64(r);
    s->missing = gateReadU64(r);
    s->dropped = gateReadU64(r);
    s->promotions = gateReadU64(r);
    s->revocations = gateReadU64(r);
    s->retired = gateReadU64(r);
    /* A share of at most GATE_COVERAGE_WINDOW requests. */
    if (s->window > GATE_COVERAGE_WINDOW || !(s->coverage >= 0.0 && s->coverage <= 1.0))
        gateReadFail(r, "coverage out of range");
}

gate *gateDecode(const unsigned char *buf, size_t len, char *err, size_t errlen) {
    gateReader r = {buf, len, 0, err, errlen};
    const unsigned char *magic = gateReadBytes(&r, 4);
    gateConfig cfg;
    gate *g = NULL;

    if (magic && memcmp(magic, "GATE", 4) != 0) gateReadFail(&r, "bad magic");
    if (gateReadInt(&r, 2) != GATE_CODEC_VERSION) gateReadFail(&r, "unsupported codec version");
    if (gateReadInt(&r, 2) != GATE_FEATURES_VERSION)
        gateReadFail(&r, "unsupported features version");

    gateReadConfig(&r, &cfg);
    if (!r.failed && gateConfigValidate(&cfg, err, errlen) != GATE_OK) r.failed = 1;
    /* gateAllocate() sizes the trainer by the class count: a blob that
     * claims a classifier must be long enough to hold one. */
    if (r.len < gateModelBytes(GATE_DIM, cfg.nclasses)) gateReadFail(&r, "truncated");
    if (!r.failed && (g = gateAllocate(&cfg)) == NULL) gateReadFail(&r, "out of memory");
    gateConfigFree(&cfg);

    if (g) gateReadState(&r, g);
    if (r.len != 0) gateReadFail(&r, "trailing bytes");
    /* Everything the fields must satisfy together: counters, the queue of
     * evidence, the generations and their policies. */
    if (!r.failed && !gateCheckInvariants(g, err, errlen)) r.failed = 1;
    if (r.failed) {
        gateFree(g);
        return NULL;
    }
    return g;
}
