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
 *   config      u8 kind (0 classifier, 1 score), f64 alpha, f64 delta,
 *               f64 audit, u32 K, K x (u32 length, label bytes),
 *               K x f64 class alpha, K x u8 class defer (0 or 1),
 *               u32 snapshot examples, i64 feedback timeout
 *   ledger      u64 lineage, u64 epoch, i64 created_ms, i64 now_ms *,
 *               u64 generations, u8 paused (0 or 1), u64 snapshot_updates,
 *               u8 blind (0 or 1), u32 missing_run, u64 answer_mark
 *   trainer     u8 present (1 for a classifier, else 0); if present, a model
 *   candidate   u8 present; if present, a generation
 *   serving     u8 present; if present, a generation
 *   requests    u64 next_seq, u32 count, then the waiting requests in
 *               ascending seq order
 *   counters    u64 requests *, accepted *, audited *, abstained *, full *,
 *               window *, f64 coverage *, u64 observations, learned,
 *               ignored, missing, dropped, promotions, revocations, retired
 *
 *   model       u64 updates, V * GATE_DIM x f32 weights, V x f32 biases
 *   generation  u64 gen, i64 frozen_ms, i64 promoted_ms, u64 samples,
 *               errors, missed, audits, audit_errors, budget, a model
 *               (classifier only), R * GATE_THRESHOLDS x region (4 x f64
 *               log wealth, u64 n, errors, certified_at),
 *               R * GATE_THRESHOLDS x u64 paired, R x drift detector
 *               (u64 asked, u64 errors, 3 x u32 wrong, 3 x u32 lost)
 *   request     u64 seq, i64 created_ms, u32 flags, i32 cand_label,
 *               i32 serv_label, f64 cand_score, f64 serv_score,
 *               u64 cand_gen, u64 serv_gen, i32 result, u64 spot, u32 n,
 *               n x u32 bucket, n x f32 value
 *
 * COUNTERS. No counter is written above GATE_COUNT_MAX (2^62) and none is
 * accepted above it. The core never moves a request number, a snapshot
 * number or the clock past its limit (it stops handing them out), and every
 * other counter is simply written as the limit once it is beyond: so
 * whatever the core makes of a gate that decoded, it can be encoded and
 * decoded again.
 *
 * The flags of a request are the GATE_OBS_* bits of gate_core.h: 1 audit,
 * 2 would-accept, 4 proof for the candidate, 8 proof for the serving
 * generation, 16 answered and waiting for its turn (RESOLVED), 32 counted
 * as an error until its answer arrives (PROVISIONAL). A label that does not
 * exist (a score gate, or no such generation when the request was asked)
 * is -1, and so is the result of a request that has no answer yet. A
 * generation number of 0 means "there was none".
 *
 * The fields marked * are written as zero for a digest. Not stored, because
 * the decoder computes them again: the bar and the policy of a generation,
 * the number of answered requests, and the evidence queue, which is the
 * waiting requests that are proof for something (4 or 8) and are not
 * PROVISIONAL, in the order of the list. Weights, regions and drift
 * detectors carry no count: it follows from the config and from GATE_DIM
 * and GATE_THRESHOLDS, so changing either of those changes the format and
 * needs a new GATE_CODEC_VERSION. So does changing what a flag bit means.
 *
 * WHAT THE DECODER ENFORCES. The limits of gate_defs.h, what the core needs
 * in order to be safe (the labels and buckets it indexes with, the numbers
 * it does arithmetic on, the counters that must not wrap around), what
 * keeps the encoding canonical, and what gateCheckInvariants() states. It
 * deliberately enforces nothing else: a gate that decodes must still decode
 * after the core has worked on it, and every further rule is one more way
 * for a state the core can reach to be refused when it is loaded again.
 * In particular it does not ask whether the proof in a blob is true, or
 * whether a waiting request could be waiting where it is (evidence for a
 * generation that is gone, a request the proof moved on without in front
 * of older proof): the core copes with all of those, and whoever restores
 * a blob it does not trust drops the proof anyway (gateRecover).
 *
 * One such way is left, and the decoder cannot close it on its own: a bound
 * has an edge. A blob may put a bounded counter, or the clock, exactly at
 * its bound. The core does not stop counting there, and takes a clock
 * beyond its range if it is handed one; what is encoded after that is
 * refused. (No gate gets there by counting or by the passing of time.)
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

/* Arrays whose size follows from these constants carry no count. */
#if GATE_CODEC_VERSION == 1 && \
    (GATE_DIM != 8192u || GATE_THRESHOLDS != 32u || GATE_NBETS != 4 || GATE_DRIFT_SCALES != 3)
#error "these constants shape the encoding: changing one needs a new GATE_CODEC_VERSION"
#endif

/* The kind of a gate and the flags of a request are stored as they are. */
#if GATE_CODEC_VERSION == 1 && \
    (GATE_KIND_CLASSIFIER != 0 || GATE_KIND_SCORE != 1 || GATE_OBS_AUDIT != 1u || \
     GATE_OBS_WOULD_ACCEPT != 2u || GATE_OBS_CAND != 4u || GATE_OBS_SERV != 8u || \
     GATE_OBS_RESOLVED != 16u || GATE_OBS_PROVISIONAL != 32u)
#error "these values are part of the encoding: changing one needs a new GATE_CODEC_VERSION"
#endif

/* Little-endian stores and loads, spelled out byte by byte: right on any
 * host, and a plain move on a little-endian one. */
static void gatePut32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void gatePut64(unsigned char *p, uint64_t v) {
    gatePut32(p, (uint32_t)v);
    gatePut32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t gateGet32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t gateGet64(const unsigned char *p) {
    return gateGet32(p) | (uint64_t)gateGet32(p + 4) << 32;
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

static void gateWriteU8(gateWriter *w, uint8_t v) {
    unsigned char *p = gateWriteRoom(w, 1);

    if (p) *p = v;
}

static void gateWriteU16(gateWriter *w, uint16_t v) {
    gateWriteU8(w, (uint8_t)v);
    gateWriteU8(w, (uint8_t)(v >> 8));
}

static void gateWriteU32(gateWriter *w, uint32_t v) {
    unsigned char *p = gateWriteRoom(w, 4);

    if (p) gatePut32(p, v);
}

static void gateWriteU64(gateWriter *w, uint64_t v) {
    unsigned char *p = gateWriteRoom(w, 8);

    if (p) gatePut64(p, v);
}

static void gateWriteI64(gateWriter *w, int64_t v) { gateWriteU64(w, (uint64_t)v); }

/* A counter: never written above the limit the decoder accepts (see
 * COUNTERS in the file comment). */
static void gateWriteCount(gateWriter *w, uint64_t v) {
    gateWriteU64(w, v > GATE_COUNT_MAX ? GATE_COUNT_MAX : v);
}

static void gateWriteDouble(gateWriter *w, double v) {
    uint64_t bits;

    memcpy(&bits, &v, sizeof(bits));
    gateWriteU64(w, bits);
}

static void gateWriteFloats(gateWriter *w, const float *v, size_t n) {
    unsigned char *p = gateWriteRoom(w, n * 4);

    if (p == NULL) return;
    for (size_t i = 0; i < n; i++, p += 4) {
        uint32_t bits;

        memcpy(&bits, &v[i], sizeof(bits));
        gatePut32(p, bits);
    }
}

/* --------------------------------- Encoder ------------------------------- */

static void gateWriteModel(gateWriter *w, const gateModel *m) {
    gateWriteCount(w, m->updates);
    gateWriteFloats(w, m->w, (size_t)m->nvec * m->dim);
    gateWriteFloats(w, m->b, m->nvec);
}

static void gateWriteGeneration(gateWriter *w, const gate *g, const gateGeneration *gen) {
    gateWriteU8(w, gen != NULL);
    if (gen == NULL) return;

    gateWriteCount(w, gen->gen);
    gateWriteI64(w, gen->frozen_ms);
    gateWriteI64(w, gen->promoted_ms);
    gateWriteCount(w, gen->samples);
    gateWriteCount(w, gen->errors);
    gateWriteCount(w, gen->missed);
    gateWriteCount(w, gen->audits);
    gateWriteCount(w, gen->audit_errors);
    gateWriteCount(w, gen->budget);
    if (g->cfg.kind == GATE_KIND_CLASSIFIER) gateWriteModel(w, &gen->model);
    for (uint32_t i = 0; i < g->nrows * GATE_THRESHOLDS; i++) {
        const gateRegion *reg = &gen->regions[i];

        for (int j = 0; j < GATE_NBETS; j++) gateWriteDouble(w, reg->logw[j]);
        gateWriteCount(w, reg->n);
        gateWriteCount(w, reg->errors);
        gateWriteCount(w, reg->certified_at);
    }
    for (uint32_t i = 0; i < g->nrows * GATE_THRESHOLDS; i++) gateWriteCount(w, gen->paired[i]);
    for (uint32_t row = 0; row < g->nrows; row++) {
        const gateDrift *d = &gen->drift[row];

        gateWriteCount(w, d->asked);
        gateWriteCount(w, d->errors);
        for (int s = 0; s < GATE_DRIFT_SCALES; s++) gateWriteU32(w, d->wrong[s]);
        for (int s = 0; s < GATE_DRIFT_SCALES; s++) gateWriteU32(w, d->lost[s]);
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
    gateWriteCount(w, o->spot);
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
    gateWriteU16(&w, GATE_CODEC_VERSION);
    gateWriteU16(&w, GATE_FEATURES_VERSION);

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
    gateWriteCount(&w, g->generations);
    gateWriteU8(&w, g->paused != 0);
    gateWriteCount(&w, g->snapshot_updates);
    gateWriteU8(&w, g->blind != 0);
    gateWriteU32(&w, g->missing_run);
    gateWriteCount(&w, g->answer_mark);

    gateWriteU8(&w, (uint8_t)classifier);
    if (classifier) gateWriteModel(&w, &g->trainer);
    gateWriteGeneration(&w, g, g->candidate);
    gateWriteGeneration(&w, g, g->serving);

    gateWriteCount(&w, g->next_seq);
    gateWriteU32(&w, g->npending);
    for (const gateObs *o = g->oldest; o; o = o->next) gateWriteObs(&w, o);

    gateWriteCount(&w, s.requests);
    gateWriteCount(&w, s.accepted);
    gateWriteCount(&w, s.audited);
    gateWriteCount(&w, s.abstained);
    gateWriteCount(&w, s.full);
    gateWriteCount(&w, s.window);
    gateWriteDouble(&w, s.coverage);
    gateWriteCount(&w, s.observations);
    gateWriteCount(&w, s.learned);
    gateWriteCount(&w, s.ignored);
    gateWriteCount(&w, s.missing);
    gateWriteCount(&w, s.dropped);
    gateWriteCount(&w, s.promotions);
    gateWriteCount(&w, s.revocations);
    gateWriteCount(&w, s.retired);

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

static uint8_t gateReadU8(gateReader *r) {
    const unsigned char *p = gateReadBytes(r, 1);

    return p ? *p : 0;
}

static uint16_t gateReadU16(gateReader *r) {
    uint8_t low = gateReadU8(r);

    return (uint16_t)(low | gateReadU8(r) << 8);
}

static uint32_t gateReadU32(gateReader *r) {
    const unsigned char *p = gateReadBytes(r, 4);

    return p ? gateGet32(p) : 0;
}

static uint64_t gateReadU64(gateReader *r) {
    const unsigned char *p = gateReadBytes(r, 8);

    return p ? gateGet64(p) : 0;
}

static int64_t gateReadI64(gateReader *r) { return (int64_t)gateReadU64(r); }

/* A counter. The encoder never writes one above GATE_COUNT_MAX and the
 * core never needs one to be (see COUNTERS in the file comment), so a
 * larger value is not ours; and held to this range a counter cannot wrap,
 * get out of order with another, or show as a negative number. */
static uint64_t gateReadCount(gateReader *r) {
    uint64_t v = gateReadU64(r);

    if (v > GATE_COUNT_MAX) gateReadFail(r, "counter out of range");
    return v;
}

static int gateReadBool(gateReader *r) {
    uint8_t v = gateReadU8(r);

    if (v > 1) gateReadFail(r, "invalid flag");
    return v == 1;
}

/* No field of a gate is ever NaN or infinite, so none is let in. */
static double gateReadDouble(gateReader *r) {
    uint64_t bits = gateReadU64(r);
    double v;

    memcpy(&v, &bits, sizeof(v));
    if (isfinite(v)) return v;
    gateReadFail(r, "non-finite number");
    return 0.0;
}

static float gateGetFloat(const unsigned char *p) {
    uint32_t bits = gateGet32(p);
    float v;

    memcpy(&v, &bits, sizeof(v));
    return v;
}

/* --------------------------------- Decoder ------------------------------- */

/* The model saturates its weights, so one beyond the limit was never
 * written by it. NaN and infinities fail the comparison too. */
static void gateReadWeights(gateReader *r, float *v, size_t n) {
    const unsigned char *p = gateReadBytes(r, n * 4);

    if (p == NULL) return;
    for (size_t i = 0; i < n; i++, p += 4) {
        v[i] = gateGetFloat(p);
        if (!(fabsf(v[i]) <= GATE_MAX_WEIGHT)) {
            gateReadFail(r, "weight out of range");
            return;
        }
    }
}

/* Fill an allocated model. */
static void gateReadModel(gateReader *r, gateModel *m) {
    m->updates = gateReadCount(r);
    gateReadWeights(r, m->w, (size_t)m->nvec * m->dim);
    gateReadWeights(r, m->b, m->nvec);
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

    gen->gen = gateReadCount(r);
    gen->frozen_ms = gateReadI64(r);
    gen->promoted_ms = gateReadI64(r);
    gen->samples = gateReadCount(r);
    gen->errors = gateReadCount(r);
    gen->missed = gateReadCount(r);
    gen->audits = gateReadCount(r);
    gen->audit_errors = gateReadCount(r);
    gen->budget = gateReadCount(r);
    if (classifier) gateReadModel(r, &gen->model);
    for (uint32_t i = 0; i < g->nrows * GATE_THRESHOLDS; i++) {
        gateRegion *reg = &gen->regions[i];

        for (int j = 0; j < GATE_NBETS; j++) reg->logw[j] = gateReadDouble(r);
        reg->n = gateReadCount(r);
        reg->errors = gateReadCount(r);
        reg->certified_at = gateReadCount(r);
    }
    for (uint32_t i = 0; i < g->nrows * GATE_THRESHOLDS; i++) gen->paired[i] = gateReadCount(r);
    for (uint32_t row = 0; row < g->nrows; row++) {
        gateDrift *d = &gen->drift[row];

        d->asked = gateReadCount(r);
        d->errors = gateReadCount(r);
        for (int s = 0; s < GATE_DRIFT_SCALES; s++) d->wrong[s] = gateReadU32(r);
        for (int s = 0; s < GATE_DRIFT_SCALES; s++) {
            d->lost[s] = gateReadU32(r);
            /* Disagreements and lost answers of the block now open: no
             * more than it holds. */
            if ((uint64_t)d->wrong[s] + d->lost[s] > gateDriftOpen(d, s))
                gateReadFail(r, "invalid drift detector");
        }
        if (d->errors > d->asked) gateReadFail(r, "invalid drift detector");
        gateGenerationRefreshPolicy(g, gen, row);
    }
    gen->log_threshold = gateBar(g, gen->gen);
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
        x->idx[i] = gateGet32(p + (size_t)i * 4);
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

/* The flags a request can rest with: all of them. */
#define GATE_OBS_STORED \
    (GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_EVIDENCE | GATE_OBS_RESOLVED | \
     GATE_OBS_PROVISIONAL)

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
    o->spot = gateReadCount(r);
    gateReadClues(r, &o->x);

    /* The core takes the age of a request from the clock, now and later:
     * the difference of two clock values, the request's never the larger.
     * With a time from before the clock's range it would overflow. */
    if (o->created_ms < 0) gateReadFail(r, "request time out of range");
    if (o->created_ms > g->now_ms) gateReadFail(r, "request from the future");
    if (o->flags & ~(uint32_t)GATE_OBS_STORED) gateReadFail(r, "invalid request flags");
    if (!gateGuessValid(g, o->cand_gen, o->cand_label) ||
        !gateGuessValid(g, o->serv_gen, o->serv_label))
        gateReadFail(r, "invalid request guess");
    answered = (o->flags & GATE_OBS_RESOLVED) != 0;
    if (answered ? (o->result < 0 || o->result >= answers) : o->result != -1)
        gateReadFail(r, "invalid request answer");
    /* A spot check was one when it was asked (the proof flag may have been
     * dropped since), and its number was handed out by the detector of its
     * row if that generation is still the one serving. */
    if (o->spot != 0) {
        const uint32_t spot_flags = GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT;

        if ((o->flags & spot_flags) != spot_flags || o->serv_gen == 0) {
            gateReadFail(r, "invalid spot check");
        } else if (!r->failed && g->serving && g->serving->gen == o->serv_gen) {
            uint32_t row = g->cfg.kind == GATE_KIND_CLASSIFIER ? (uint32_t)o->serv_label : 0;

            if (o->spot > g->serving->drift[row].asked) gateReadFail(r, "invalid spot check");
        }
    }
}

static void gateReadPending(gateReader *r, gate *g) {
    uint32_t count = gateReadU32(r);
    uint64_t last = 0;

    for (uint32_t i = 0; i < count && !r->failed; i++) {
        gateObs *o;

        /* The core records a request only while the gate is not full, so
         * none follows the one that filled it, however many of the older
         * ones have been answered since. With the limit below this is also
         * what bounds the memory a blob can make us allocate. */
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
        /* Answered requests do not count towards full, but the core lets
         * only so many of them wait for an older answer: with one more,
         * proof moves on (gatePass) and they are applied. */
        if (g->nanswered > GATE_MAX_WAITING) gateReadFail(r, "too many answers waiting");
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
    g->snapshot_updates = gateReadCount(r);
    g->blind = gateReadBool(r);
    g->missing_run = gateReadU32(r);
    g->answer_mark = gateReadCount(r);
    /* The run ends in an alarm when it reaches the limit. */
    if (g->missing_run >= GATE_MISSING_RUN) gateReadFail(r, "counter out of range");
    /* The range the core is specified for (TIME in gate_core.h), the same
     * that every later clock value is held to. The time of each request is
     * checked against it. */
    if (g->now_ms < 0 || g->now_ms > GATE_MAX_CLOCK_MS) gateReadFail(r, "clock out of range");

    if (gateReadBool(r) != classifier) gateReadFail(r, "trainer does not match the gate kind");
    if (classifier) gateReadModel(r, &g->trainer);
    g->candidate = gateReadGeneration(r, g);
    g->serving = gateReadGeneration(r, g);

    g->next_seq = gateReadCount(r);
    /* Request numbers start at 1; the mark is a value next_seq once had. */
    if (g->next_seq == 0 || g->answer_mark > g->next_seq)
        gateReadFail(r, "invalid request counter");
    gateReadPending(r, g);

    s->requests = gateReadCount(r);
    s->accepted = gateReadCount(r);
    s->audited = gateReadCount(r);
    s->abstained = gateReadCount(r);
    s->full = gateReadCount(r);
    s->window = gateReadCount(r);
    s->coverage = gateReadDouble(r);
    s->observations = gateReadCount(r);
    s->learned = gateReadCount(r);
    s->ignored = gateReadCount(r);
    s->missing = gateReadCount(r);
    s->dropped = gateReadCount(r);
    s->promotions = gateReadCount(r);
    s->revocations = gateReadCount(r);
    s->retired = gateReadCount(r);
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
    if (gateReadU16(&r) != GATE_CODEC_VERSION) gateReadFail(&r, "unsupported codec version");
    if (gateReadU16(&r) != GATE_FEATURES_VERSION)
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
