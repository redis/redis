/*
 * Gate data type: the gate itself. See gate_core.h for the rules.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "gate_core.h"

#define T GATE_THRESHOLDS

/* --------------------------------- Names --------------------------------- */

const char *gateStateName(gateStateId s) {
    switch (s) {
    case GATE_STATE_COLD: return "COLD";
    case GATE_STATE_CALIBRATING: return "CALIBRATING";
    case GATE_STATE_ACTIVE: return "ACTIVE";
    case GATE_STATE_PAUSED: return "PAUSED";
    }
    return "UNKNOWN";
}

const char *gateActionName(int action) {
    switch (action) {
    case GATE_ACTION_ACCEPT: return "ACCEPT";
    case GATE_ACTION_ABSTAIN: return "ABSTAIN";
    case GATE_ACTION_AUDIT: return "AUDIT";
    }
    return "UNKNOWN";
}

/* ----------------------------- Configuration ----------------------------- */

static char *gateStrdupLen(const char *s, size_t len) {
    char *copy = gate_malloc(len + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

void gateConfigInit(gateConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->kind = GATE_KIND_SCORE;
    cfg->delta = GATE_DEFAULT_DELTA;
    cfg->audit = GATE_DEFAULT_AUDIT;
    cfg->snapshot_examples = GATE_DEFAULT_SNAPSHOT_EXAMPLES;
    cfg->feedback_timeout_ms = GATE_DEFAULT_FEEDBACK_TIMEOUT_MS;
}

void gateConfigFree(gateConfig *cfg) {
    if (cfg == NULL) return;
    if (cfg->labels) {
        for (uint32_t i = 0; i < cfg->nclasses; i++) gate_free(cfg->labels[i]);
        gate_free(cfg->labels);
    }
    gate_free(cfg->class_alpha);
    gate_free(cfg->class_defer);
    memset(cfg, 0, sizeof(*cfg));
}

int gateConfigSetClasses(gateConfig *cfg, const char *const *labels,
                         const size_t *lens, uint32_t n)
{
    if (cfg->labels || cfg->class_alpha || cfg->class_defer || n == 0) return GATE_ERR;
    cfg->labels = gate_calloc(n, sizeof(char *));
    cfg->class_alpha = gate_calloc(n, sizeof(double));
    cfg->class_defer = gate_calloc(n, 1);
    if (!cfg->labels || !cfg->class_alpha || !cfg->class_defer) goto oom;
    cfg->nclasses = n; /* set first so that a partial failure frees them */
    for (uint32_t i = 0; i < n; i++) {
        cfg->labels[i] = gateStrdupLen(labels[i], lens[i]);
        if (cfg->labels[i] == NULL) goto oom;
    }
    return GATE_OK;

oom:
    if (cfg->labels) {
        for (uint32_t i = 0; i < n; i++) gate_free(cfg->labels[i]);
    }
    gate_free(cfg->labels);
    gate_free(cfg->class_alpha);
    gate_free(cfg->class_defer);
    cfg->labels = NULL;
    cfg->class_alpha = NULL;
    cfg->class_defer = NULL;
    cfg->nclasses = 0;
    return GATE_ERR;
}

int gateConfigCopy(gateConfig *dst, const gateConfig *src) {
    *dst = *src;
    dst->labels = NULL;
    dst->class_alpha = NULL;
    dst->class_defer = NULL;
    dst->nclasses = 0;
    if (src->nclasses == 0) return GATE_OK;

    size_t lens[GATE_MAX_CLASSES];
    if (src->nclasses > GATE_MAX_CLASSES || src->labels == NULL) return GATE_ERR;
    for (uint32_t i = 0; i < src->nclasses; i++) lens[i] = strlen(src->labels[i]);
    if (gateConfigSetClasses(dst, (const char *const *)src->labels, lens,
                             src->nclasses) != GATE_OK)
        return GATE_ERR;
    if (src->class_alpha)
        memcpy(dst->class_alpha, src->class_alpha, sizeof(double) * src->nclasses);
    if (src->class_defer) memcpy(dst->class_defer, src->class_defer, src->nclasses);
    return GATE_OK;
}

int gateConfigLabelIndex(const gateConfig *cfg, const char *label, size_t len) {
    for (uint32_t i = 0; i < cfg->nclasses; i++) {
        if (strlen(cfg->labels[i]) == len && memcmp(cfg->labels[i], label, len) == 0)
            return (int)i;
    }
    return -1;
}

#define GATE_CFG_FAIL(...) do { \
    if (err && errlen) snprintf(err, errlen, __VA_ARGS__); \
    return GATE_ERR; \
} while (0)

int gateConfigValidate(gateConfig *cfg, char *err, size_t errlen) {
    if (cfg->kind != GATE_KIND_CLASSIFIER && cfg->kind != GATE_KIND_SCORE)
        GATE_CFG_FAIL("invalid gate kind");
    /* The comparisons are written so that NaN fails them. */
    if (!(cfg->alpha > 0.0 && cfg->alpha < 1.0))
        GATE_CFG_FAIL("ALPHA must be greater than 0 and less than 1");
    if (!(cfg->delta > 0.0 && cfg->delta < 1.0))
        GATE_CFG_FAIL("DELTA must be greater than 0 and less than 1");
    if (!(cfg->audit > 0.0 && cfg->audit <= 1.0))
        GATE_CFG_FAIL("AUDIT must be greater than 0 and at most 1");
    if (cfg->snapshot_examples < 1)
        GATE_CFG_FAIL("the snapshot step must be at least 1 example");
    if (cfg->feedback_timeout_ms < 1 || cfg->feedback_timeout_ms > GATE_MAX_FEEDBACK_TIMEOUT_MS)
        GATE_CFG_FAIL("the feedback timeout is out of range");

    uint32_t k = cfg->nclasses;
    if (cfg->kind == GATE_KIND_SCORE) {
        if (k != 0 || cfg->labels || cfg->class_alpha || cfg->class_defer)
            GATE_CFG_FAIL("a SCORE gate has no classes");
        return GATE_OK;
    }

    if (k < GATE_MIN_CLASSES || k > GATE_MAX_CLASSES)
        GATE_CFG_FAIL("CLASSES must list between %d and %d labels",
                      GATE_MIN_CLASSES, GATE_MAX_CLASSES);
    if (!cfg->labels || !cfg->class_alpha || !cfg->class_defer)
        GATE_CFG_FAIL("missing class configuration");
    uint32_t answered = 0;
    for (uint32_t i = 0; i < k; i++) {
        const char *l = cfg->labels[i];
        if (l == NULL || !gateValidName(l, strlen(l), GATE_MAX_LABEL_BYTES))
            GATE_CFG_FAIL("labels must be 1 to %u bytes of UTF-8 without "
                          "control characters", GATE_MAX_LABEL_BYTES);
        for (uint32_t j = 0; j < i; j++)
            if (strcmp(cfg->labels[j], l) == 0) GATE_CFG_FAIL("duplicate label '%s'", l);
        if (cfg->class_alpha[i] == 0.0) cfg->class_alpha[i] = cfg->alpha;
        if (!(cfg->class_alpha[i] > 0.0 && cfg->class_alpha[i] <= cfg->alpha))
            GATE_CFG_FAIL("CLASSALPHA for '%s' must be greater than 0 and at most "
                          "ALPHA", l);
        if (cfg->class_defer[i] > 1) GATE_CFG_FAIL("invalid ALWAYSDEFER flag");
        if (!cfg->class_defer[i]) answered++;
    }
    if (answered == 0) GATE_CFG_FAIL("ALWAYSDEFER cannot cover every label");
    return GATE_OK;
}

/* ------------------------------ Small helpers ---------------------------- */

static int gateIsClassifier(const gate *g) { return g->cfg.kind == GATE_KIND_CLASSIFIER; }

double gateRowAlpha(const gate *g, uint32_t row) {
    return gateIsClassifier(g) ? g->cfg.class_alpha[row] : g->cfg.alpha;
}

int gateRowTested(const gate *g, uint32_t row) {
    return gateIsClassifier(g) ? !g->cfg.class_defer[row] : 1;
}

int gateIsEntry(const gate *g, uint32_t t) {
    /* Classifier: the most confident threshold and every eighth below it.
     * A confident model proves the top first and walks down; the lower
     * entry points keep a less confident model from waiting on a region it
     * rarely reaches. Score gates know nothing about the score's scale, so
     * every threshold stands on its own. */
    return gateIsClassifier(g) ? ((T - 1 - t) % 8) == 0 : 1;
}

uint32_t gateEntryPoints(const gate *g) {
    uint32_t n = 0;
    for (uint32_t t = 0; t < T; t++) n += gateIsEntry(g, t);
    return n;
}

uint32_t gateTestedRows(const gate *g) {
    uint32_t n = 0;
    for (uint32_t r = 0; r < g->nrows; r++) n += gateRowTested(g, r);
    return n;
}

gateStateId gateState(const gate *g) {
    if (g->paused) return GATE_STATE_PAUSED;
    if (g->serving) return GATE_STATE_ACTIVE;
    if (g->candidate) return GATE_STATE_CALIBRATING;
    return GATE_STATE_COLD;
}

double gateRowThreshold(const gate *g, uint32_t row) {
    if (g->serving == NULL || row >= g->nrows) return -1.0;
    int32_t t = g->serving->policy[row];
    return t < 0 ? -1.0 : g->grid[t];
}

/* Evidence of a generation that fell inside its current policy. */
static uint64_t gatePolicySamples(const gate *g, const gateGeneration *gen) {
    uint64_t n = 0;
    for (uint32_t r = 0; r < g->nrows; r++)
        if (gen->policy[r] >= 0) n += gen->regions[r * T + gen->policy[r]].n;
    return n;
}

double gateCoverage(const gate *g) {
    return g->serving ? g->stats.coverage : 0.0;
}

/* The serving generation changed or went away: its coverage average
 * starts over. */
static void gateCoverageReset(gate *g) {
    g->stats.window = 0;
    g->stats.coverage = 0.0;
}

double gateBudgetSpentNow(const gate *g) {
    return gateBudgetSpent(g->cfg.delta, g->generations);
}

/* ------------------------------ Construction ----------------------------- */

static uint32_t gateSlot(uint64_t seq) {
    /* Fibonacci hashing: waiting ids that share low bits (say, every 4096th
     * request left unanswered) must not pile up in one chain. */
    return (uint32_t)((seq * 0x9e3779b97f4a7c15ull) >> 32) & (GATE_TABLE_SIZE - 1);
}

gateGeneration *gateGenerationAllocate(const gate *g) {
    gateGeneration *gen = gate_calloc(1, sizeof(*gen));
    if (gen == NULL) return NULL;
    gen->regions = gate_malloc(sizeof(gateRegion) * g->nrows * T);
    gen->policy = gate_malloc(sizeof(int32_t) * g->nrows);
    gen->drift = gate_malloc(sizeof(gateDrift) * g->nrows);
    if (!gen->regions || !gen->policy || !gen->drift) {
        gateGenerationRelease(gen);
        return NULL;
    }
    for (uint32_t i = 0; i < g->nrows * T; i++) gateRegionInit(&gen->regions[i]);
    for (uint32_t r = 0; r < g->nrows; r++) {
        gen->policy[r] = -1;
        gateDriftInit(&gen->drift[r]);
    }
    return gen;
}

void gateGenerationRelease(gateGeneration *gen) {
    if (gen == NULL) return;
    gateModelFree(&gen->model);
    gate_free(gen->regions);
    gate_free(gen->policy);
    gate_free(gen->drift);
    gate_free(gen);
}

gateObs *gateObsAllocate(void) {
    gateObs *o = gate_calloc(1, sizeof(*o));
    if (o == NULL) return NULL;
    o->result = -1;
    o->cand_label = -1;
    o->serv_label = -1;
    return o;
}

void gateObsRelease(gateObs *o) {
    if (o == NULL) return;
    gateSparseFree(&o->x);
    gate_free(o);
}

void gateObsLink(gate *g, gateObs *o) {
    uint32_t slot = gateSlot(o->seq);

    o->hnext = g->table[slot];
    g->table[slot] = o;
    o->next = NULL;
    o->prev = g->newest;
    if (g->newest) g->newest->next = o; else g->oldest = o;
    g->newest = o;
    g->npending++;
    g->pending_bytes += GATE_OBS_BYTES(o->x.n);
    o->enext = NULL;
    if (o->flags & GATE_OBS_EVIDENCE) {
        if (g->etail) g->etail->enext = o; else g->ehead = o;
        g->etail = o;
        g->nevidence++;
    }
}

/* Remove a request from the list and the index. It must not be in the
 * evidence queue any more. */
static void gateObsUnlink(gate *g, gateObs *o) {
    gateObs **pp = &g->table[gateSlot(o->seq)];

    while (*pp && *pp != o) pp = &(*pp)->hnext;
    if (*pp) *pp = o->hnext;
    if (o->prev) o->prev->next = o->next; else g->oldest = o->next;
    if (o->next) o->next->prev = o->prev; else g->newest = o->prev;
    g->npending--;
    g->pending_bytes -= GATE_OBS_BYTES(o->x.n);
    o->prev = o->next = o->hnext = NULL;
}

/* Take the head off the evidence queue. */
static gateObs *gateEvidencePop(gate *g) {
    gateObs *o = g->ehead;

    g->ehead = o->enext;
    if (g->ehead == NULL) g->etail = NULL;
    o->enext = NULL;
    g->nevidence--;
    return o;
}

/* Clues are only needed until the request has been trained on. */
static void gateObsDropClues(gate *g, gateObs *o) {
    g->pending_bytes -= GATE_SPARSE_BYTES(o->x.n);
    gateSparseFree(&o->x);
}

static void gateDropAllPending(gate *g) {
    gateObs *o = g->oldest;

    while (o) {
        gateObs *next = o->next;
        gateObsRelease(o);
        o = next;
    }
    memset(g->table, 0, sizeof(gateObs *) * GATE_TABLE_SIZE);
    g->oldest = g->newest = g->ehead = g->etail = NULL;
    g->npending = g->nevidence = 0;
    g->pending_bytes = 0;
}

gateObs *gateFindObservation(const gate *g, uint64_t seq) {
    gateObs *o = g->table[gateSlot(seq)];

    while (o && o->seq != seq) o = o->hnext;
    return o;
}

gate *gateAllocate(const gateConfig *cfg) {
    gate *g = gate_calloc(1, sizeof(*g));
    if (g == NULL) return NULL;
    if (gateConfigCopy(&g->cfg, cfg) != GATE_OK) goto fail;

    int classifier = cfg->kind == GATE_KIND_CLASSIFIER;
    g->nrows = classifier ? cfg->nclasses : 1;

    /* Classifier scores are the largest of K probabilities: never below
     * 1/K. Thresholds under 0.25 would only matter with many classes and
     * a guess that unsure is never worth answering. Score gates cover the
     * whole range, 0 meaning "answer everything". */
    double lo = 0.0, hi = 0.998;
    if (classifier) {
        lo = 1.0 / (double)cfg->nclasses;
        if (lo < 0.25) lo = 0.25;
        hi = 0.999;
    }
    if (gateGridBuild(g->grid, lo, hi) != GATE_OK) goto fail;

    g->bets = gate_malloc(sizeof(gateBets) * g->nrows);
    g->drift_bets = gate_malloc(sizeof(gateDriftBets) * g->nrows);
    g->table = gate_calloc(GATE_TABLE_SIZE, sizeof(gateObs *));
    if (!g->bets || !g->drift_bets || !g->table) goto fail;
    for (uint32_t r = 0; r < g->nrows; r++) {
        gateBetsInit(&g->bets[r], gateRowAlpha(g, r));
        gateDriftBetsInit(&g->drift_bets[r], gateRowAlpha(g, r));
    }
    if (classifier && gateModelInit(&g->trainer, GATE_DIM, cfg->nclasses) != GATE_OK)
        goto fail;
    g->next_seq = 1;
    return g;

fail:
    gateFree(g);
    return NULL;
}

void gateFree(gate *g) {
    if (g == NULL) return;
    if (g->table) gateDropAllPending(g);
    gateGenerationRelease(g->candidate);
    gateGenerationRelease(g->serving);
    gateModelFree(&g->trainer);
    gate_free(g->bets);
    gate_free(g->drift_bets);
    gate_free(g->table);
    gateConfigFree(&g->cfg);
    gate_free(g);
}

static uint64_t gateGenerationBytes(const gate *g, const gateGeneration *gen) {
    uint64_t n = sizeof(*gen);
    n += (uint64_t)g->nrows * T * sizeof(gateRegion);
    n += (uint64_t)g->nrows * (sizeof(int32_t) + sizeof(gateDrift));
    if (gen->model.w) n += gateModelBytes(gen->model.dim, gen->model.nclasses);
    return n;
}

uint64_t gateMemoryUsage(const gate *g) {
    uint64_t n = sizeof(*g);
    for (uint32_t i = 0; i < g->cfg.nclasses; i++)
        n += sizeof(char *) + strlen(g->cfg.labels[i]) + 1 + sizeof(double) + 1;
    n += (uint64_t)g->nrows * (sizeof(gateBets) + sizeof(gateDriftBets));
    n += (uint64_t)GATE_TABLE_SIZE * sizeof(gateObs *);
    if (g->trainer.w) n += gateModelBytes(g->trainer.dim, g->trainer.nclasses);
    if (g->candidate) n += gateGenerationBytes(g, g->candidate);
    if (g->serving) n += gateGenerationBytes(g, g->serving);
    return n + g->pending_bytes;
}

uint64_t gateFreeEffort(const gate *g) {
    return 1 + g->npending + (g->candidate != NULL) + (g->serving != NULL);
}

/* ------------------------------- Generations ----------------------------- */

void gateGenerationRefreshPolicy(const gate *g, gateGeneration *gen, uint32_t row) {
    int32_t lowest = -1;
    int above = 0; /* is the next more confident region proven? */

    for (int32_t t = (int32_t)T - 1; t >= 0; t--) {
        int crossed = gen->regions[row * T + (uint32_t)t].certified_at != 0;
        int proven = crossed && (gateIsEntry(g, (uint32_t)t) || above);

        if (proven) lowest = t;
        above = proven;
    }
    gen->policy[row] = lowest;
}

static int gateHasPolicy(const gate *g, const gateGeneration *gen) {
    for (uint32_t r = 0; r < g->nrows; r++)
        if (gen->policy[r] >= 0) return 1;
    return 0;
}

/* Feed one error indicator to every region of the guessed row that
 * contains the score. */
static void gateGenerationApply(gate *g, gateGeneration *gen, int32_t label,
                                double score, int error)
{
    gen->samples++;
    if (error) gen->errors++;

    uint32_t row = gateIsClassifier(g) ? (uint32_t)label : 0;
    if (gateIsClassifier(g) && (label < 0 || (uint32_t)label >= g->nrows)) return;
    if (!gateRowTested(g, row)) return;

    uint32_t count = gateGridCount(g->grid, T, score);
    gateRegion *regions = &gen->regions[row * T];
    for (uint32_t t = 0; t < count; t++) gateRegionUpdate(&regions[t], &g->bets[row], error);

    /* Wealth only grows on a correct guess, so only then can a region
     * cross. The first crossing is latched: by Ville's inequality "ever
     * reached the bar" is the event the budget pays for. */
    if (error) return;
    int crossed = 0;
    for (uint32_t t = 0; t < count; t++) {
        if (regions[t].certified_at == 0 && gateRegionCertifies(&regions[t], gen->log_threshold)) {
            regions[t].certified_at = regions[t].n;
            crossed = 1;
        }
    }
    if (crossed) gateGenerationRefreshPolicy(g, gen, row);
}

static void gatePromote(gate *g) {
    gateGenerationRelease(g->serving);
    g->serving = g->candidate;
    g->candidate = NULL;
    g->serving->promoted_ms = g->now_ms;
    g->serving->paired = 0;
    g->serving->budget = 0;
    g->stats.promotions++;
    gateCoverageReset(g);
}

/* The drift detector fired: the present no longer looks like the traffic
 * the proof was collected on. Nothing learned or proven before can be
 * trusted, so everything goes and the gate starts over. */
static void gateDriftAlarm(gate *g) {
    gateGenerationRelease(g->serving);
    gateGenerationRelease(g->candidate);
    g->serving = NULL;
    g->candidate = NULL;
    if (gateIsClassifier(g)) gateModelReset(&g->trainer);
    g->snapshot_updates = 0;
    g->stats.revocations++;
    gateCoverageReset(g);
}

/* Promote the candidate if it earned it, drop it if it ran out of budget. */
static void gateConsiderCandidate(gate *g) {
    gateGeneration *c = g->candidate;

    if (c == NULL) return;
    if (gateHasPolicy(g, c) &&
        (g->serving == NULL || gatePolicySamples(g, c) > c->paired))
    {
        gatePromote(g);
        return;
    }
    if (c->budget && c->samples >= c->budget) {
        gateGenerationRelease(c);
        g->candidate = NULL;
        g->stats.retired++;
    }
}

/* A piece of evidence is live while a generation it is proof for exists. */
static int gateObsLive(const gate *g, const gateObs *o) {
    if (o->flags & GATE_OBS_CAND) {
        if (g->candidate && g->candidate->gen == o->cand_gen) return 1;
        if (g->serving && g->serving->gen == o->cand_gen) return 1;
    }
    if ((o->flags & GATE_OBS_SERV) && g->serving && g->serving->gen == o->serv_gen) return 1;
    return 0;
}

static void gateApplyEvidence(gate *g, const gateObs *o) {
    int missing = (o->flags & GATE_OBS_MISSING) != 0;
    int err_cand, err_serv;

    if (missing) {
        err_cand = err_serv = 1;
        g->stats.missing++;
    } else if (gateIsClassifier(g)) {
        err_cand = o->cand_label != o->result;
        err_serv = o->serv_label != o->result;
    } else {
        err_cand = err_serv = !o->result;
    }

    if (o->flags & GATE_OBS_CAND) {
        if (g->candidate && g->candidate->gen == o->cand_gen) {
            gateGenerationApply(g, g->candidate, o->cand_label, o->cand_score, err_cand);
            if ((o->flags & GATE_OBS_WOULD_ACCEPT) && g->serving &&
                g->serving->gen == o->serv_gen)
                g->candidate->paired++;
            gateConsiderCandidate(g);
        } else if (g->serving && g->serving->gen == o->cand_gen) {
            /* Asked while this generation was the candidate, answered after
             * its promotion: still proof for it. */
            gateGenerationApply(g, g->serving, o->cand_label, o->cand_score, err_cand);
        }
    }

    if ((o->flags & GATE_OBS_SERV) && g->serving && g->serving->gen == o->serv_gen) {
        gateGeneration *s = g->serving;

        gateGenerationApply(g, s, o->serv_label, o->serv_score, err_serv);
        if ((o->flags & GATE_OBS_WOULD_ACCEPT) && (o->flags & GATE_OBS_AUDIT)) {
            uint32_t row = gateIsClassifier(g) ? (uint32_t)o->serv_label : 0;

            s->audits++;
            if (err_serv) s->audit_errors++;
            if (gateDriftPush(&s->drift[row], &g->drift_bets[row], err_serv,
                              log(GATE_DRIFT_ALARM)))
                gateDriftAlarm(g);
        }
    }
}

/* Apply answered evidence in request order, as far as possible. */
static void gateDrain(gate *g) {
    gateObs *h;

    while ((h = g->ehead) != NULL) {
        if (!gateObsLive(g, h)) {
            /* The generations it was proof for are gone. */
            gateEvidencePop(g);
            if (h->flags & GATE_OBS_RESOLVED) {
                if (h->flags & GATE_OBS_MISSING) g->stats.dropped++;
                gateObsUnlink(g, h);
                gateObsRelease(h);
            } else if (gateIsClassifier(g)) {
                h->flags &= ~(uint32_t)GATE_OBS_EVIDENCE; /* still good for training */
            } else {
                g->stats.dropped++;
                gateObsUnlink(g, h);
                gateObsRelease(h);
            }
            continue;
        }
        if (!(h->flags & GATE_OBS_RESOLVED)) break;
        gateEvidencePop(g);
        gateObsUnlink(g, h);
        gateApplyEvidence(g, h);
        gateObsRelease(h);
    }
}

static int gateSnapshotDue(const gate *g) {
    if (g->candidate) return 0;
    if (gateIsClassifier(g))
        return g->trainer.updates - g->snapshot_updates >= g->cfg.snapshot_examples;
    return g->serving == NULL;
}

/* Freeze a new candidate generation and spend its share of the budget. */
static void gateSnapshot(gate *g) {
    gateGeneration *gen = gateGenerationAllocate(g);

    if (gen == NULL) return; /* out of memory: try again at the next call */
    if (gateIsClassifier(g) && gateModelCopy(&gen->model, &g->trainer) != GATE_OK) {
        gateGenerationRelease(gen);
        return;
    }
    g->generations++;
    gen->gen = g->generations;
    gen->frozen_ms = g->now_ms;
    gen->log_threshold = gateBudgetLogThreshold(g->cfg.delta, gen->gen,
                                                gateTestedRows(g) * gateEntryPoints(g));
    if (gateIsClassifier(g)) {
        /* A flawless region of row r reaches the bar after bar / (its best
         * per-sample gain) answers. A snapshot that has consumed many times
         * that much proof without taking over is not going to: the trainer
         * has moved on, try its next snapshot instead. Score gates have no
         * better snapshot to wait for, so theirs never run out. */
        double need = 0;
        for (uint32_t r = 0; r < g->nrows; r++) {
            if (!gateRowTested(g, r)) continue;
            double n = gen->log_threshold / g->bets[r].inc_ok[GATE_NBETS - 1];
            if (n > need) need = n;
        }
        gen->budget = (uint64_t)ceil(need) * GATE_CANDIDATE_BUDGET_FACTOR;
        if (gen->budget == 0) gen->budget = 1;
    }
    g->snapshot_updates = g->trainer.updates;
    g->candidate = gen;
}

/* Bring the gate to rest after any change: apply what can be applied, then
 * take a snapshot if one is due. */
static void gateSettle(gate *g) {
    gateDrain(g);
    if (gateSnapshotDue(g)) gateSnapshot(g);
}

/* ------------------------------- Maintenance ----------------------------- */

int gateMaintenanceDue(const gate *g, int64_t now_ms) {
    int64_t now = now_ms > g->now_ms ? now_ms : g->now_ms;

    if (g->oldest && g->oldest->created_ms + g->cfg.feedback_timeout_ms <= now) return 1;
    return gateSnapshotDue(g);
}

void gateMaintain(gate *g, int64_t now_ms) {
    gateObs *o;

    if (now_ms > g->now_ms) g->now_ms = now_ms;
    while ((o = g->oldest) != NULL &&
           o->created_ms + g->cfg.feedback_timeout_ms <= g->now_ms)
    {
        if (o->flags & GATE_OBS_EVIDENCE) {
            /* It is the oldest waiting request, so it is the head of the
             * evidence queue. Unanswered: it becomes a missing answer and
             * is applied as an error when drained. Answered: it was only
             * waiting behind older evidence, which is gone now. */
            if (!(o->flags & GATE_OBS_RESOLVED)) {
                o->flags |= GATE_OBS_MISSING | GATE_OBS_RESOLVED;
                gateObsDropClues(g, o);
            }
            gateDrain(g);
            if (g->oldest == o) break; /* cannot happen; never spin */
        } else {
            g->stats.dropped++;
            gateObsUnlink(g, o);
            gateObsRelease(o);
        }
    }
    gateSettle(g);
}

int gateFull(const gate *g) {
    return g->npending >= GATE_MAX_PENDING || g->pending_bytes >= GATE_MAX_PENDING_BYTES;
}

/* ------------------------------- Prediction ------------------------------ */

int gateEvaluate(const gate *g, const gateSparse *x, double ext_score, gateEval *ev) {
    memset(ev, 0, sizeof(*ev));
    ev->serving.label = ev->candidate.label = -1;

    if (!gateIsClassifier(g)) {
        if (g->serving) {
            ev->has_serving = 1;
            ev->serving.score = ext_score;
        }
        if (g->candidate) {
            ev->has_candidate = 1;
            ev->candidate.score = ext_score;
        }
        return GATE_OK;
    }
    if (g->serving) {
        if (x == NULL) return GATE_ERR;
        ev->serving.label = gateModelPredict(&g->serving->model, x, NULL, &ev->serving.score);
        if (ev->serving.label < 0) return GATE_ERR;
        ev->has_serving = 1;
    }
    if (g->candidate) {
        if (x == NULL) return GATE_ERR;
        ev->candidate.label =
            gateModelPredict(&g->candidate->model, x, NULL, &ev->candidate.score);
        if (ev->candidate.label < 0) return GATE_ERR;
        ev->has_candidate = 1;
    }
    return GATE_OK;
}

typedef struct gatePlan {
    int action;
    int32_t label;   /* serving label (ACCEPT) */
    uint32_t flags;  /* GATE_OBS_* for the request */
    int store;       /* record the request */
} gatePlan;

/* What gatePredict() does with a request, without doing it. The gate must
 * have been maintained and must not be full. */
static int gatePlanRequest(const gate *g, double u, const gateEval *ev, gatePlan *p) {
    int classifier = gateIsClassifier(g);
    int audit = u < g->cfg.audit;
    int would = 0;

    p->action = GATE_ACTION_ABSTAIN;
    p->label = -1;
    p->flags = audit ? GATE_OBS_AUDIT : 0;
    p->store = 0;

    if (g->serving) {
        if (!ev->has_serving) return GATE_ERR;
        int32_t label = ev->serving.label;
        if (classifier && (label < 0 || (uint32_t)label >= g->nrows)) return GATE_ERR;
        uint32_t row = classifier ? (uint32_t)label : 0;
        int32_t t = g->serving->policy[row];

        if (gateRowTested(g, row)) {
            if (t >= 0 && ev->serving.score >= g->grid[t]) would = 1;
            /* A row without a proven threshold sends everything to the
             * teacher, so everything is unbiased proof for it. Once it
             * answers, only the audited share is. */
            if (t < 0 || audit) p->flags |= GATE_OBS_SERV;
        }
        if (would) {
            p->flags |= GATE_OBS_WOULD_ACCEPT;
            p->label = label;
        }
    }
    if (g->candidate) {
        if (!ev->has_candidate) return GATE_ERR;
        if (classifier &&
            (ev->candidate.label < 0 || (uint32_t)ev->candidate.label >= g->nrows))
            return GATE_ERR;
        /* The candidate needs a sample that does not depend on what the
         * serving generation decided: everything while nothing serves,
         * the audited share afterwards. */
        if (g->serving == NULL || audit) p->flags |= GATE_OBS_CAND;
    }

    if (would && !audit && !g->paused) {
        p->action = GATE_ACTION_ACCEPT;
        return GATE_OK;
    }
    if (would && audit && !g->paused) p->action = GATE_ACTION_AUDIT;
    p->label = -1;
    if (p->flags & GATE_OBS_EVIDENCE)
        p->store = 1;
    else
        p->store = classifier && g->npending < GATE_TRAIN_PENDING;
    return GATE_OK;
}

int gatePredictNeedsFeatures(const gate *g, double u, const gateEval *ev) {
    gatePlan p;

    if (!gateIsClassifier(g) || gateFull(g)) return 0;
    if (gatePlanRequest(g, u, ev, &p) != GATE_OK) return 0;
    return p.store;
}

int gatePredict(gate *g, int64_t now_ms, double u, const gateEval *ev,
                gateSparse *x, gatePredictResult *res)
{
    gatePlan p;

    gateMaintain(g, now_ms);
    res->action = GATE_ACTION_ABSTAIN;
    res->label = -1;
    res->seq = 0;

    /* A full gate does not look at the request at all: whether it is full
     * was decided by earlier requests, so leaving this one out cannot bias
     * anything. */
    if (gateFull(g)) {
        g->stats.requests++;
        g->stats.abstained++;
        g->stats.full++;
        return GATE_OK;
    }
    if (gatePlanRequest(g, u, ev, &p) != GATE_OK) return GATE_ERR;
    if (p.store && gateIsClassifier(g) && x == NULL) return GATE_ERR;

    g->stats.requests++;
    if (g->serving) {
        /* Running average: plain mean until the window is full, then an
         * exponential one with the window as its memory. */
        if (g->stats.window < GATE_COVERAGE_WINDOW) g->stats.window++;
        double inside = (p.flags & GATE_OBS_WOULD_ACCEPT) ? 1.0 : 0.0;
        g->stats.coverage += (inside - g->stats.coverage) / (double)g->stats.window;
    }
    res->action = p.action;
    if (p.action == GATE_ACTION_ACCEPT) {
        g->stats.accepted++;
        res->label = p.label;
        return GATE_OK;
    }
    if (p.action == GATE_ACTION_AUDIT) g->stats.audited++; else g->stats.abstained++;
    if (!p.store) return GATE_OK;

    gateObs *o = gateObsAllocate();
    if (o == NULL) return GATE_OK; /* out of memory: abstain without an id */
    o->seq = g->next_seq++;
    o->created_ms = g->now_ms;
    o->flags = p.flags;
    if (g->serving) {
        o->serv_gen = g->serving->gen;
        o->serv_label = ev->serving.label;
        o->serv_score = ev->serving.score;
    }
    if (g->candidate) {
        o->cand_gen = g->candidate->gen;
        o->cand_label = ev->candidate.label;
        o->cand_score = ev->candidate.score;
    }
    if (x) {
        o->x = *x;
        x->n = 0;
        x->idx = NULL;
        x->val = NULL;
    }
    gateObsLink(g, o);
    g->stats.observations++;
    res->seq = o->seq;
    return GATE_OK;
}

/* ------------------------------- Feedback -------------------------------- */

static void gateTrainStep(gate *g, const gateSparse *x, int32_t label) {
    double q[GATE_MAX_CLASSES];

    for (uint32_t k = 0; k < g->cfg.nclasses; k++) q[k] = 0.0;
    q[label] = 1.0;
    /* A rejected (non-finite) step leaves the model untouched; with
     * validated clues and saturating weights it cannot happen. */
    (void)gateModelLearn(&g->trainer, x, q, GATE_LEARNING_RATE, 0.0, NULL);
}

int gateLearn(gate *g, int64_t now_ms, uint64_t epoch, uint64_t seq, int32_t result) {
    gateMaintain(g, now_ms);

    gateObs *o = NULL;
    if (epoch == g->epoch && seq != 0 && seq < g->next_seq) o = gateFindObservation(g, seq);
    if (o == NULL || (o->flags & GATE_OBS_RESOLVED)) {
        g->stats.ignored++;
        return 0;
    }

    o->result = result;
    if (gateIsClassifier(g)) {
        gateTrainStep(g, &o->x, result);
        gateObsDropClues(g, o);
    }
    g->stats.learned++;
    if (o->flags & GATE_OBS_EVIDENCE) {
        o->flags |= GATE_OBS_RESOLVED; /* applied when its turn comes */
    } else {
        gateObsUnlink(g, o);
        gateObsRelease(o);
    }
    gateSettle(g);
    return 1;
}

int gateTrain(gate *g, int64_t now_ms, const gateSparse *x, int32_t label) {
    double q[GATE_MAX_CLASSES];
    int ret;

    gateMaintain(g, now_ms);
    if (!gateIsClassifier(g) || label < 0 || (uint32_t)label >= g->cfg.nclasses)
        return GATE_ERR;
    for (uint32_t k = 0; k < g->cfg.nclasses; k++) q[k] = 0.0;
    q[label] = 1.0;
    ret = gateModelLearn(&g->trainer, x, q, GATE_LEARNING_RATE, 0.0, NULL);
    gateSettle(g);
    return ret;
}

/* ------------------------------ Administration --------------------------- */

gate *gateCreate(const gateConfig *cfg, uint64_t lineage, uint64_t epoch, int64_t now_ms) {
    gate *g = gateAllocate(cfg);

    if (g == NULL) return NULL;
    g->lineage = lineage;
    g->epoch = epoch;
    g->created_ms = now_ms;
    g->now_ms = now_ms;
    gateSettle(g); /* a score gate starts testing right away */
    return g;
}

void gatePause(gate *g, int64_t now_ms) {
    gateMaintain(g, now_ms);
    g->paused = 1;
}

void gateResume(gate *g, int64_t now_ms) {
    gateMaintain(g, now_ms);
    g->paused = 0;
}

static void gateDropProof(gate *g, uint64_t new_epoch) {
    gateDropAllPending(g);
    gateGenerationRelease(g->candidate);
    gateGenerationRelease(g->serving);
    g->candidate = NULL;
    g->serving = NULL;
    g->snapshot_updates = 0;
    g->epoch = new_epoch;
    gateCoverageReset(g);
}

void gateReset(gate *g, int64_t now_ms, uint64_t new_epoch) {
    if (now_ms > g->now_ms) g->now_ms = now_ms;
    gateDropProof(g, new_epoch);
    if (gateIsClassifier(g)) gateModelReset(&g->trainer);
    memset(&g->stats, 0, sizeof(g->stats));
    gateSettle(g);
}

void gateRecover(gate *g, int64_t now_ms, uint64_t new_epoch) {
    if (now_ms > g->now_ms) g->now_ms = now_ms;
    gateDropProof(g, new_epoch);
    gateSettle(g);
}

/* ------------------------------ Invariants ------------------------------- */

#define GATE_INV_FAIL(...) do { \
    if (err && errlen) snprintf(err, errlen, __VA_ARGS__); \
    return 0; \
} while (0)

static int gateCheckGeneration(const gate *g, const gateGeneration *gen,
                               const char *what, char *err, size_t errlen)
{
    if (gen->gen == 0 || gen->gen > g->generations)
        GATE_INV_FAIL("%s: generation number out of range", what);
    for (uint32_t r = 0; r < g->nrows; r++) {
        gateGeneration copy = *gen;
        int32_t policy = gen->policy[r], expect;
        int32_t tmp[GATE_MAX_CLASSES];

        /* Recompute the row's policy from the crossed regions. */
        copy.policy = tmp;
        gateGenerationRefreshPolicy(g, &copy, r);
        expect = tmp[r];
        if (policy != expect)
            GATE_INV_FAIL("%s: row %u policy %d, expected %d", what, r, policy, expect);
        if (!gateRowTested(g, r)) {
            for (uint32_t t = 0; t < T; t++)
                if (gen->regions[r * T + t].n != 0)
                    GATE_INV_FAIL("%s: deferred row %u has proof", what, r);
        }
        for (uint32_t t = 0; t < T; t++) {
            const gateRegion *reg = &gen->regions[r * T + t];
            if (reg->errors > reg->n || reg->n > gen->samples || reg->certified_at > reg->n)
                GATE_INV_FAIL("%s: region counters inconsistent", what);
            if (t > 0 && reg->n > gen->regions[r * T + t - 1].n)
                GATE_INV_FAIL("%s: regions are not nested", what);
        }
    }
    if (gen->errors > gen->samples) GATE_INV_FAIL("%s: errors exceed samples", what);
    if (gen->audit_errors > gen->audits) GATE_INV_FAIL("%s: audit errors exceed audits", what);
    return 1;
}

int gateCheckInvariants(const gate *g, char *err, size_t errlen) {
    uint32_t n = 0, ne = 0, indexed = 0;
    uint64_t bytes = 0, last = 0;
    const gateObs *o, *prev = NULL;

    for (o = g->oldest; o; prev = o, o = o->next) {
        if (o->prev != prev) GATE_INV_FAIL("broken back link at seq %llu",
                                           (unsigned long long)o->seq);
        if (o->seq <= last) GATE_INV_FAIL("requests not in ascending order");
        if (o->seq >= g->next_seq) GATE_INV_FAIL("request beyond next_seq");
        if (gateFindObservation(g, o->seq) != o) GATE_INV_FAIL("request missing from index");
        if ((o->flags & GATE_OBS_RESOLVED) && !(o->flags & GATE_OBS_EVIDENCE))
            GATE_INV_FAIL("answered request outside the evidence queue");
        if (!gateIsClassifier(g) && o->x.n != 0) GATE_INV_FAIL("score gate stores clues");
        last = o->seq;
        bytes += GATE_OBS_BYTES(o->x.n);
        n++;
    }
    if (g->newest != prev) GATE_INV_FAIL("newest pointer is wrong");
    if (n != g->npending) GATE_INV_FAIL("npending %u, counted %u", g->npending, n);
    if (bytes != g->pending_bytes) GATE_INV_FAIL("pending_bytes is wrong");

    last = 0;
    prev = NULL;
    for (o = g->ehead; o; prev = o, o = o->enext) {
        if (!(o->flags & GATE_OBS_EVIDENCE)) GATE_INV_FAIL("non-evidence in the queue");
        if (o->seq <= last) GATE_INV_FAIL("evidence queue out of order");
        if (gateFindObservation(g, o->seq) != o) GATE_INV_FAIL("queued request not waiting");
        last = o->seq;
        ne++;
    }
    if (g->etail != prev) GATE_INV_FAIL("etail pointer is wrong");
    if (ne != g->nevidence) GATE_INV_FAIL("nevidence %u, counted %u", g->nevidence, ne);
    if (g->ehead && (g->ehead->flags & GATE_OBS_RESOLVED) && gateObsLive(g, g->ehead))
        GATE_INV_FAIL("answered live evidence left at the head");

    for (uint32_t i = 0; i < GATE_TABLE_SIZE; i++)
        for (o = g->table[i]; o; o = o->hnext) indexed++;
    if (indexed != g->npending) GATE_INV_FAIL("index holds %u requests", indexed);

    if (g->candidate) {
        if (!gateCheckGeneration(g, g->candidate, "candidate", err, errlen)) return 0;
        if (g->candidate->promoted_ms != 0) GATE_INV_FAIL("candidate has a promotion time");
        if (g->serving && g->serving->gen == g->candidate->gen)
            GATE_INV_FAIL("candidate and serving share a number");
    }
    if (g->serving) {
        if (!gateCheckGeneration(g, g->serving, "serving", err, errlen)) return 0;
        if (!gateHasPolicy(g, g->serving)) GATE_INV_FAIL("serving has no proven threshold");
    }
    if (!gateIsClassifier(g) && g->trainer.w != NULL) GATE_INV_FAIL("score gate has a model");
    if (g->stats.requests != g->stats.accepted + g->stats.audited + g->stats.abstained)
        GATE_INV_FAIL("request counters do not add up");
    return 1;
}
