/*
 * Gate data type: the gate state machine.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * The protocol is specified in gate_core.h. Notes on how this file honours
 * it:
 *
 * THREE VIEWS OF THE PENDING OBSERVATIONS. Every pending observation is in
 * the pending list (oldest .. newest, ascending seq, which is also ascending
 * created_ms because the clock never moves backwards) and in the seq index.
 * The eligible ones are in the FIFO as well, and "is in the FIFO" is exactly
 * "has GATE_OBS_CAND or GATE_OBS_SERV set": the flags are cleared the moment
 * an observation leaves the FIFO without being consumed. Observations only
 * ever leave the FIFO from its head, so a singly linked queue is enough.
 *
 * EVIDENCE ORDER. Evidence reaches a generation in two ways only, and both
 * hold the head of the FIFO: gateDrain() applies a labelled head, and
 * gateMissing() imputes an error for an unlabelled head that ran out of
 * time. A labelled observation queued behind an unlabelled one that some
 * live generation is still waiting for stays queued. The one exception is a
 * request that could not be stored at all under ONMISSING ERROR: its error
 * is imputed at once, ahead of the queue. An error applied early can only
 * lower every later wealth, so this is the conservative direction.
 *
 * WHO DRAINS. Removing a generation can leave the head of the FIFO with no
 * generation to wait for, which unblocks whatever is queued behind it.
 * gateRevoke(), gateDiscardCandidate() and gatePromote() do not drain
 * themselves: they run in the middle of applying an observation. Instead
 * every entry point drains before it returns. So between two calls the head
 * of the FIFO, if there is one, is unlabelled and still awaited by a live
 * generation; gateCheckInvariants() verifies exactly that.
 *
 * NEVER LEAVE THE VALID SET. A gate must survive encode -> decode, and the
 * decoder only accepts what gateConfigValidate() and gateCheckInvariants()
 * accept. So arguments that would take the state out of that set (a class
 * index that is not a class, a non-finite score, a malformed feature vector,
 * an invalid version string, a configuration that no longer fits MAXMEMORY)
 * are refused before anything is changed, even where the header leaves their
 * validation to the caller. For valid arguments this is invisible.
 *
 * TIME. Clocks are caller supplied and TTLs can be ten years, so deadlines
 * are computed with saturating arithmetic: a deadline that does not fit in
 * 64 bits means "never", not undefined behaviour.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "gate_core.h"
#include "gate_internal.h"

/* Rejecting NaN / Inf scores relies on isfinite() really classifying its
 * argument, which -ffast-math (-ffinite-math-only) lets the compiler skip. */
#if defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__
#error "gate_core.c must not be compiled with -ffast-math / -ffinite-math-only"
#endif

/* ---------------------------------- Time ---------------------------------- */

/* t + ttl for a duration ttl >= 0, saturating at INT64_MAX. */
static int64_t gateDeadline(int64_t t, int64_t ttl) {
    if (ttl <= 0) return t;
    return t > INT64_MAX - ttl ? INT64_MAX : t + ttl;
}

/* now - then, clamped to [0, INT64_MAX]. */
static int64_t gateElapsed(int64_t now, int64_t then) {
    if (now <= then) return 0;
    if (then < 0 && now > INT64_MAX + then) return INT64_MAX;
    return now - then;
}

/* ------------------------------ Small accessors --------------------------- */

double gateRowAlpha(const gate *g, uint32_t row) {
    if (g->cfg.class_mode && row < g->cfg.nclasses) return g->cfg.class_alpha[row];
    return g->cfg.alpha;
}

double gateBudgetSpentNow(const gate *g) {
    return gateBudgetSpent(g->cfg.delta, g->generations);
}

double gateBudgetRemainingNow(const gate *g) {
    /* The generation count is converted before the addition: g + 1 must not
     * wrap around for a ledger at its maximum. */
    return g->cfg.delta / ((double)g->generations + 1.0);
}

gateStateId gateState(const gate *g) {
    if (g->paused) return GATE_STATE_PAUSED;
    if (g->serving != NULL) return GATE_STATE_ACTIVE;
    if (g->recovery_required) return GATE_STATE_REVOKED;
    if (g->candidate != NULL) return GATE_STATE_CALIBRATING;
    if (g->revoked_reason != GATE_REASON_NONE) return GATE_STATE_REVOKED;
    return GATE_STATE_COLD;
}

gateReason gateIdleReason(const gate *g) {
    if (g->recovery_required) return GATE_REASON_RECOVERY_REQUIRED;
    if (g->paused) return GATE_REASON_PAUSED;
    if (g->serving != NULL) return GATE_REASON_NONE;
    if (g->revoked_reason != GATE_REASON_NONE) return g->revoked_reason;
    return GATE_REASON_COLD_START;
}

double gateAuditProbability(const gate *g) {
    return g->serving != NULL ? g->cfg.audit : g->cfg.cold_audit;
}

/* ------------------------------ Generations ------------------------------- */

gateGeneration *gateGenerationAllocate(const gate *g) {
    uint32_t rows = g->nrows, T = g->cfg.nthresholds, i;
    size_t nregions = (size_t)rows * T;
    gateGeneration *gen = gate_calloc(1, sizeof(*gen));

    if (gen == NULL) return NULL;
    gen->regions = gate_malloc(nregions * sizeof(gateRegion));
    gen->policy = gate_malloc(rows * sizeof(int32_t));
    gen->drift = gate_malloc(rows * sizeof(gateDriftWindow));
    if (gen->regions == NULL || gen->policy == NULL || gen->drift == NULL) {
        gateGenerationRelease(gen);
        return NULL;
    }
    for (i = 0; i < nregions; i++) gateRegionInit(&gen->regions[i]);
    for (i = 0; i < rows; i++) {
        gen->policy[i] = -1;
        gateDriftInit(&gen->drift[i]);
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

/* Requested bytes of one live generation. Must mirror the per-generation
 * term of gateFixedBytes(), which reserves room for two of these. */
static uint64_t gateGenerationBytes(const gate *g, const gateGeneration *gen) {
    uint64_t rows = g->nrows, T = g->cfg.nthresholds;
    uint64_t bytes = sizeof(gateGeneration);

    if (gen->model.w != NULL) bytes += gateModelBytes(gen->model.dim, gen->model.nclasses);
    bytes += rows * T * sizeof(gateRegion);
    bytes += rows * sizeof(int32_t);
    bytes += rows * sizeof(gateDriftWindow);
    return bytes;
}

/* 1 if at least one row of the generation has a certified threshold. */
static int gateGenerationHasPolicy(const gate *g, const gateGeneration *gen) {
    uint32_t row;

    for (row = 0; row < g->nrows; row++)
        if (gen->policy[row] >= 0) return 1;
    return 0;
}

double gateGenerationCoverage(const gate *g, const gateGeneration *gen) {
    uint32_t T = g->cfg.nthresholds, row;
    uint64_t inside = 0;

    if (gen == NULL || gen->samples == 0) return 0.0;
    /* Regions are nested, so the region at a row's policy threshold counted
     * every applied sample of that row the policy would have accepted. */
    for (row = 0; row < g->nrows; row++) {
        if (gen->policy[row] < 0) continue;
        inside += gen->regions[(size_t)row * T + (uint32_t)gen->policy[row]].n;
    }
    return (double)inside / (double)gen->samples;
}

/* ------------------------------ Observations ------------------------------ */

gateObs *gateObsAllocate(void) {
    gateObs *o = gate_calloc(1, sizeof(*o));

    if (o == NULL) return NULL;
    o->result = o->cand_label = o->serv_label = -1;
    return o;
}

void gateObsRelease(gateObs *o) {
    if (o == NULL) return;
    gateSparseFree(&o->x);
    gate_free(o);
}

gateObs *gateFindObservation(const gate *g, uint64_t seq) {
    gateObs *o;

    if (g->table == NULL || g->table_size == 0) return NULL;
    for (o = g->table[gateTableSlot(seq, g->table_size)]; o != NULL; o = o->hnext)
        if (o->seq == seq) return o;
    return NULL;
}

void gateObsLink(gate *g, gateObs *o) {
    gateObs **bucket = &g->table[gateTableSlot(o->seq, g->table_size)];

    o->hnext = *bucket;
    *bucket = o;

    o->prev = g->newest;
    o->next = NULL;
    if (g->newest != NULL) g->newest->next = o; else g->oldest = o;
    g->newest = o;

    o->enext = NULL;
    if (o->flags & GATE_OBS_ELIGIBLE) {
        if (g->etail != NULL) g->etail->enext = o; else g->ehead = o;
        g->etail = o;
        g->neligible++;
    }
    g->npending++;
    g->pending_bytes += GATE_OBS_BYTES(o->x.n);
}

/* Take the head off the eligible FIFO. It stays pending and keeps its flags:
 * the caller either consumes it or clears them. */
static gateObs *gateFifoPop(gate *g) {
    gateObs *h = g->ehead;

    g->ehead = h->enext;
    if (g->ehead == NULL) g->etail = NULL;
    h->enext = NULL;
    g->neligible--;
    return h;
}

/* Remove an observation from the pending list and the index. It must not be
 * in the FIFO any more. */
static void gateObsUnlink(gate *g, gateObs *o) {
    gateObs **link = &g->table[gateTableSlot(o->seq, g->table_size)];

    while (*link != NULL && *link != o) link = &(*link)->hnext;
    if (*link != NULL) *link = o->hnext;
    o->hnext = NULL;

    if (o->prev != NULL) o->prev->next = o->next; else g->oldest = o->next;
    if (o->next != NULL) o->next->prev = o->prev; else g->newest = o->prev;
    o->prev = o->next = NULL;

    g->npending--;
    g->pending_bytes -= GATE_OBS_BYTES(o->x.n);
}

/* Drop every pending observation and make their ids INVALIDATED. Emptying
 * the bucket of each observation as it goes empties the whole index, since
 * every entry is being removed: O(pending), not O(index size). */
static void gateDropPending(gate *g) {
    gateObs *o = g->oldest, *next;

    for (; o != NULL; o = next) {
        next = o->next;
        g->table[gateTableSlot(o->seq, g->table_size)] = NULL;
        gateObsRelease(o);
    }
    g->oldest = g->newest = NULL;
    g->ehead = g->etail = NULL;
    g->npending = 0;
    g->neligible = 0;
    g->pending_bytes = 0;
    g->invalid_below = g->next_seq;
}

int64_t gateOldestPendingAge(const gate *g) {
    if (g->oldest == NULL) return 0;
    return gateElapsed(g->now_ms, g->oldest->created_ms);
}

/* -------------------------------- Lifecycle ------------------------------- */

gate *gateAllocate(const gateConfig *cfg) {
    gate *g;
    uint32_t T, row;
    int classifier;

    /* Everything below, and every function of this file, relies on the
     * ranges a validated configuration guarantees (at least one row and two
     * thresholds, at most GATE_MAX_CLASSES classes for the stack buffers,
     * non-NULL version strings). Re-checking once per gate is cheap, and
     * turns a caller that skipped validation into a clean NULL. */
    if (cfg == NULL || gateConfigValidate(cfg, NULL, 0) != GATE_OK) return NULL;

    g = gate_calloc(1, sizeof(*g));
    if (g == NULL) return NULL;
    if (gateConfigCopy(&g->cfg, cfg) != GATE_OK) goto fail;
    T = g->cfg.nthresholds;
    classifier = g->cfg.mode == GATE_MODE_CLASSIFIER;
    g->nrows = gateConfigRows(&g->cfg);

    g->grid = gate_malloc(T * sizeof(double));
    if (g->grid == NULL) goto fail;
    if (gateGridBuild(g->grid, T, classifier ? GATE_GRID_LOGIT : GATE_GRID_UNIFORM,
                      g->cfg.nclasses) != GATE_OK)
        goto fail;

    g->bets = gate_malloc(g->nrows * sizeof(gateBets));
    if (g->bets == NULL) goto fail;
    for (row = 0; row < g->nrows; row++) gateBetsInit(&g->bets[row], gateRowAlpha(g, row));

    if (classifier &&
        gateModelInit(&g->trainer, gateSchemaDim(&g->cfg.schema), g->cfg.nclasses) != GATE_OK)
        goto fail;

    g->table_size = gateTableSize(g->cfg.max_pending);
    g->table = gate_calloc(g->table_size, sizeof(gateObs *));
    if (g->table == NULL) goto fail;

    g->fixed_bytes = gateFixedBytes(&g->cfg);
    g->next_seq = 1;
    return g;

fail:
    /* Everything not built yet is still zero from the calloc, which is what
     * gateFree() expects of the parts it has nothing to release for. */
    gateFree(g);
    return NULL;
}

gate *gateCreate(const gateConfig *cfg, uint64_t lineage, uint64_t epoch, int64_t now_ms) {
    gate *g = gateAllocate(cfg);

    if (g == NULL) return NULL;
    g->lineage = lineage;
    g->epoch = epoch;
    g->created_ms = now_ms;
    g->now_ms = now_ms;
    return g;
}

void gateFree(gate *g) {
    gateObs *o, *next;

    if (g == NULL) return;
    for (o = g->oldest; o != NULL; o = next) {
        next = o->next;
        gateObsRelease(o);
    }
    gateGenerationRelease(g->candidate);
    gateGenerationRelease(g->serving);
    gateModelFree(&g->trainer);
    gate_free(g->table);
    gate_free(g->bets);
    gate_free(g->grid);
    gateConfigFree(&g->cfg);
    gate_free(g);
}

uint64_t gateMemoryUsage(const gate *g) {
    const gateConfig *cfg = &g->cfg;
    uint64_t k = cfg->nclasses, bytes = sizeof(gate);
    uint32_t i;

    /* The sizes requested for what is allocated right now, term by term in
     * the order of gateFixedBytes(). That function charges the same terms
     * with two generations whether they exist or not, hence the bound
     * usage <= fixed_bytes + pending_bytes, with equality exactly when a
     * candidate and a serving generation are both alive. */
    if (cfg->labels != NULL) {
        bytes += k * sizeof(char *);
        for (i = 0; i < cfg->nclasses; i++) bytes += strlen(cfg->labels[i]) + 1;
    }
    if (cfg->teacher != NULL) bytes += strlen(cfg->teacher) + 1;
    if (cfg->schema_version != NULL) bytes += strlen(cfg->schema_version) + 1;
    if (cfg->class_alpha != NULL) bytes += k * sizeof(double);
    if (cfg->class_defer != NULL) bytes += k;
    if (g->grid != NULL) bytes += (uint64_t)cfg->nthresholds * sizeof(double);
    if (g->bets != NULL) bytes += (uint64_t)g->nrows * sizeof(gateBets);
    if (g->table != NULL) bytes += (uint64_t)g->table_size * sizeof(gateObs *);
    if (g->trainer.w != NULL) bytes += gateModelBytes(g->trainer.dim, g->trainer.nclasses);
    if (g->candidate != NULL) bytes += gateGenerationBytes(g, g->candidate);
    if (g->serving != NULL) bytes += gateGenerationBytes(g, g->serving);
    return bytes + g->pending_bytes;
}

uint64_t gateFreeEffort(const gate *g) {
    return 1 + (uint64_t)g->npending + (g->candidate != NULL) + (g->serving != NULL);
}

/* --------------------------- Generation events ---------------------------- *
 * None of these drains the FIFO: see "WHO DRAINS" at the top of the file. */

static void gateDiscardCandidate(gate *g) {
    if (g->candidate == NULL) return;
    gateGenerationRelease(g->candidate);
    g->candidate = NULL;
    g->stats.candidates_discarded++;
}

static void gateRevoke(gate *g, gateReason reason) {
    if (g->serving == NULL) return;
    g->last_revoked_reason = reason;
    g->last_revoked_ms = g->now_ms;
    g->last_revoked_gen = g->serving->gen;
    gateGenerationRelease(g->serving);
    g->serving = NULL;
    g->revoked_reason = reason;
    g->stats.revocations++;
}

/* The candidate certified its first region: it replaces whatever is serving.
 * The generation it replaces was not found wanting, so that is not counted
 * or recorded as a revocation. */
static void gatePromote(gate *g) {
    gateGeneration *gen = g->candidate;

    gateGenerationRelease(g->serving);
    g->serving = gen;
    g->candidate = NULL;
    gen->promoted_ms = g->now_ms;
    gen->expires_ms = gateDeadline(g->now_ms, g->cfg.cert_ttl_ms);
    gen->closed = g->cfg.expand ? 0 : 1;
    g->revoked_reason = GATE_REASON_NONE;
    g->stats.promotions++;
}

/* -------------------------------- Evidence -------------------------------- */

/* Feed one frozen prediction and its error indicator to a generation. */
static void gateGenerationApply(const gate *g, gateGeneration *gen, int32_t label,
                                double score, int error, int imputed)
{
    uint32_t T = g->cfg.nthresholds, row, count, t;
    gateRegion *regions;
    int32_t lowest = -1;

    gen->samples++;
    if (error) gen->errors++;
    if (imputed) gen->imputed++;

    /* An ALWAYSDEFER class is traffic the policy can never cover: it counts
     * in the denominator of the coverage estimate and in no region. */
    row = g->cfg.class_mode ? (uint32_t)label : 0;
    if (!gateRowTested(&g->cfg, row)) return;

    regions = gen->regions + (size_t)row * T;
    count = gateGridCount(g->grid, T, score);
    for (t = 0; t < count; t++) gateRegionUpdate(&regions[t], &g->bets[row], error);

    /* An error lowers all four wealths of every region it touches, so only
     * a correct sample can take a region across the threshold. Certification
     * is sticky: certified_at records the first crossing and later samples
     * never withdraw it. */
    if (error) return;
    for (t = 0; t < count; t++) {
        if (regions[t].certified_at != 0) continue;
        if (!gateRegionCertifies(&regions[t], gen->log_threshold)) continue;
        regions[t].certified_at = regions[t].n;
        if (lowest < 0) lowest = (int32_t)t;
    }
    /* The policy is the lowest certified threshold of the row. Regions never
     * lose their certification, so it can only move down, and only to a
     * region certified just now. */
    if (lowest >= 0 && (gen->policy[row] < 0 || lowest < gen->policy[row])) {
        gen->policy[row] = lowest;
        gen->policy_updates++;
    }
}

/* Apply a resolved observation to the generations that made its stored
 * predictions. The caller holds the head of the FIFO (or a request that
 * never got in), which is what keeps evidence in observation order. */
static void gateApplyEvidence(gate *g, const gateObs *o) {
    int imputed = (o->flags & GATE_OBS_IMPUTED) != 0;
    int e_cand, e_serv;

    if (imputed) {
        e_cand = e_serv = 1;
    } else if (g->cfg.mode == GATE_MODE_SCORE) {
        /* The CORRECT bit judges the one external result both generations
         * "predicted". Anything but a clean 1 is a disagreement. */
        e_cand = e_serv = o->result != 1;
    } else {
        e_cand = o->cand_label != o->result;
        e_serv = o->serv_label != o->result;
    }

    /* The prediction of the generation that was the candidate at prediction
     * time. That generation may have been promoted since: it is then the
     * serving one, and takes the sample only while it is still testing. */
    if (o->flags & GATE_OBS_CAND) {
        if (g->candidate != NULL && g->candidate->gen == o->cand_gen) {
            gateGenerationApply(g, g->candidate, o->cand_label, o->cand_score, e_cand, imputed);
            if (gateGenerationHasPolicy(g, g->candidate)) gatePromote(g);
        } else if (g->serving != NULL && g->serving->gen == o->cand_gen) {
            if (g->cfg.expand && !g->serving->closed)
                gateGenerationApply(g, g->serving, o->cand_label, o->cand_score, e_cand, imputed);
        }
    }

    /* The prediction of the generation that was serving at prediction time.
     * A promotion a few lines above replaced it, in which case the test
     * below fails; and cand_gen != serv_gen, so no generation is ever fed
     * the same observation twice. */
    if ((o->flags & GATE_OBS_SERV) && g->serving != NULL && g->serving->gen == o->serv_gen) {
        gateGeneration *gen = g->serving;

        if (g->cfg.expand && !gen->closed)
            gateGenerationApply(g, gen, o->serv_label, o->serv_score, e_serv, imputed);
        if (o->flags & GATE_OBS_WOULD_ACCEPT) {
            uint32_t row = g->cfg.class_mode ? (uint32_t)o->serv_label : 0;
            double alpha = gateRowAlpha(g, row);
            double margin = g->cfg.drift_margin < 0 ? alpha : g->cfg.drift_margin;

            if (gateDriftPush(&gen->drift[row], e_serv, alpha + margin))
                gateRevoke(g, GATE_REASON_DRIFT_DETECTED);
        }
    }
}

/* An eligible observation is live while a generation it was sampled for
 * still exists. */
static int gateObsLive(const gate *g, const gateObs *o) {
    const gateGeneration *cand = g->candidate, *serv = g->serving;

    if (o->flags & GATE_OBS_CAND) {
        if (cand != NULL && cand->gen == o->cand_gen) return 1;
        if (serv != NULL && serv->gen == o->cand_gen) return 1;
    }
    if ((o->flags & GATE_OBS_SERV) && serv != NULL && serv->gen == o->serv_gen) return 1;
    return 0;
}

/* Advance the FIFO as far as it goes: apply labelled heads in order, and
 * demote unlabelled heads that no live generation is waiting for to
 * training-only observations. Stops at the first unlabelled, live head. */
static void gateDrain(gate *g) {
    gateObs *h;

    while ((h = g->ehead) != NULL) {
        if (h->flags & GATE_OBS_RESOLVED) {
            /* Unlinked before it is applied: applying may promote or revoke,
             * and the loop then re-reads the head against the generations
             * that are left. */
            gateFifoPop(g);
            gateObsUnlink(g, h);
            gateApplyEvidence(g, h);
            gateObsRelease(h);
        } else if (!gateObsLive(g, h)) {
            gateFifoPop(g);
            h->flags &= ~(uint32_t)GATE_OBS_ELIGIBLE;
        } else {
            break;
        }
    }
}

/* The ONMISSING policy for an eligible observation whose label will never
 * come: the head of the FIFO at its TTL, or a stack-local stand-in for a
 * request that could not be stored. */
static void gateMissing(gate *g, gateObs *o) {
    if (g->cfg.on_missing == GATE_ONMISSING_ERROR) {
        o->flags |= GATE_OBS_IMPUTED | GATE_OBS_RESOLVED;
        g->stats.imputed++;
        gateApplyEvidence(g, o);
        return;
    }

    /* REVOKE. A sample stream with a hole is no longer the stream the test
     * was promised. For a candidate that is the end. For a serving
     * generation the certifications already made stand (they depended only
     * on earlier samples), so a hole in its testing stream merely ends the
     * testing, while a hole in the audits of what it accepts ends the
     * serving: nobody is checking the accepted traffic any more. */
    if (o->flags & GATE_OBS_CAND) {
        if (g->candidate != NULL && g->candidate->gen == o->cand_gen)
            gateDiscardCandidate(g);
        else if (g->serving != NULL && g->serving->gen == o->cand_gen)
            g->serving->closed = 1;
    }
    if ((o->flags & GATE_OBS_SERV) && g->serving != NULL && g->serving->gen == o->serv_gen) {
        if (o->flags & GATE_OBS_WOULD_ACCEPT)
            gateRevoke(g, GATE_REASON_FEEDBACK_INTEGRITY);
        else
            g->serving->closed = 1;
    }
}

/* ------------------------------- Maintenance ------------------------------ */

void gateMaintain(gate *g, int64_t now_ms) {
    int64_t now, ttl = g->cfg.observation_ttl_ms;
    gateObs *o;

    if (now_ms > g->now_ms) g->now_ms = now_ms;
    now = g->now_ms;

    if (g->serving != NULL && now >= g->serving->expires_ms) {
        gateRevoke(g, GATE_REASON_CERT_EXPIRED);
        /* Before looking at expired observations: an audit of the generation
         * that just went away is not missing evidence for anything. */
        gateDrain(g);
    }

    /* The pending list is in creation order, so the expired observations
     * are a prefix of it. */
    while ((o = g->oldest) != NULL && gateDeadline(o->created_ms, ttl) <= now) {
        if (o->seq > g->expired_through) g->expired_through = o->seq;
        if (o->flags & GATE_OBS_RESOLVED) {
            /* Labelled in time and waiting for its turn: its TTL no longer
             * applies. It can only be waiting for an older eligible
             * observation, which would be ahead of it in this list too, so
             * the oldest observation is never one of these after a drain.
             * Should that ever fail, stop rather than spin. */
            gateDrain(g);
            if (g->oldest == o) break;
        } else if (o->flags & GATE_OBS_ELIGIBLE) {
            /* The oldest pending observation, hence the head of the FIFO. */
            g->stats.expired++;
            g->stats.expired_eligible++;
            gateMissing(g, o);
            gateFifoPop(g);
            gateObsUnlink(g, o);
            gateObsRelease(o);
            gateDrain(g);
        } else {
            g->stats.expired++;
            gateObsUnlink(g, o);
            gateObsRelease(o);
        }
    }
    gateDrain(g);
}

void gatePreview(const gate *g, int64_t now_ms, int fenced,
                 gateStateId *state, gateReason *reason)
{
    gateStateId st;
    gateReason why;

    if (fenced || g->recovery_required) {
        st = g->paused ? GATE_STATE_PAUSED : GATE_STATE_REVOKED;
        why = GATE_REASON_RECOVERY_REQUIRED;
    } else if (g->paused) {
        st = GATE_STATE_PAUSED;
        why = GATE_REASON_PAUSED;
    } else {
        int64_t t = now_ms > g->now_ms ? now_ms : g->now_ms;
        int64_t ttl = g->cfg.observation_ttl_ms;
        int serving = g->serving != NULL, candidate = g->candidate != NULL;
        gateReason revoked = GATE_REASON_NONE;
        const gateObs *o;

        /* Same order as gateMaintain(): the certificate first, so that an
         * unanswered audit of an expired certificate is not reported as a
         * feedback failure. */
        if (serving && t >= g->serving->expires_ms) {
            serving = 0;
            revoked = GATE_REASON_CERT_EXPIRED;
        }
        if (g->cfg.on_missing == GATE_ONMISSING_REVOKE) {
            for (o = g->oldest; o != NULL && (serving || candidate); o = o->next) {
                if (gateDeadline(o->created_ms, ttl) > t) break;
                if (o->flags & GATE_OBS_RESOLVED) continue;
                if (serving && (o->flags & GATE_OBS_SERV) && (o->flags & GATE_OBS_WOULD_ACCEPT) &&
                    o->serv_gen == g->serving->gen)
                {
                    serving = 0;
                    revoked = GATE_REASON_FEEDBACK_INTEGRITY;
                }
                if (candidate && (o->flags & GATE_OBS_CAND) && o->cand_gen == g->candidate->gen)
                    candidate = 0;
            }
        }

        if (serving) {
            st = GATE_STATE_ACTIVE;
            why = GATE_REASON_NONE;
        } else {
            if (revoked == GATE_REASON_NONE) revoked = g->revoked_reason;
            why = revoked != GATE_REASON_NONE ? revoked : GATE_REASON_COLD_START;
            if (candidate) st = GATE_STATE_CALIBRATING;
            else if (revoked != GATE_REASON_NONE) st = GATE_STATE_REVOKED;
            else st = GATE_STATE_COLD;
        }
    }
    if (state != NULL) *state = st;
    if (reason != NULL) *reason = why;
}

/* ------------------------------- Prediction ------------------------------- */

/* A feature vector the trainer can be stepped on, and that the decoder will
 * take back: strictly increasing indices below 'dim' (distinct indices are
 * what makes the model's two-pass update atomic) and finite values. */
static int gateSparseUsable(const gateSparse *x, uint32_t dim) {
    uint32_t j;

    if (x == NULL || x->n > dim) return 0;
    if (x->n != 0 && (x->idx == NULL || x->val == NULL)) return 0;
    for (j = 0; j < x->n; j++) {
        if (x->idx[j] >= dim) return 0;
        if (j > 0 && x->idx[j] <= x->idx[j - 1]) return 0;
        if (!isfinite(x->val[j])) return 0;
    }
    return 1;
}

/* 1 if 'label' is something a prediction of this gate can carry. */
static int gateLabelUsable(const gate *g, int32_t label) {
    return g->cfg.nclasses == 0 || (label >= 0 && (uint32_t)label < g->cfg.nclasses);
}

static void gateEvalClear(gateEval *ev) {
    ev->has_serving = ev->has_candidate = 0;
    ev->serving.label = ev->candidate.label = -1;
    ev->serving.score = ev->candidate.score = 0.0;
}

static int gateEvaluateOne(const gate *g, const gateGeneration *gen, const gateSparse *x,
                           double ext_score, int32_t ext_label, gatePrediction *p)
{
    if (g->cfg.mode == GATE_MODE_CLASSIFIER) {
        int label = gateModelPredict(&gen->model, x, NULL, &p->score);

        if (label < 0 || !isfinite(p->score)) return GATE_ERR;
        p->label = label;
    } else {
        p->label = g->cfg.nclasses != 0 ? ext_label : -1;
        p->score = ext_score;
    }
    return GATE_OK;
}

int gateEvaluate(const gate *g, const gateSparse *x, double ext_score,
                 int32_t ext_label, gateEval *ev)
{
    gateEvalClear(ev);
    if (g->serving == NULL && g->candidate == NULL) return GATE_OK;

    if (g->cfg.mode == GATE_MODE_CLASSIFIER) {
        if (!gateSparseUsable(x, g->trainer.dim)) return GATE_ERR;
    } else {
        /* The external scorer is the model here: a value it cannot have
         * produced is refused like a non-finite model output. */
        if (!isfinite(ext_score) || !gateLabelUsable(g, ext_label)) return GATE_ERR;
    }
    if (g->serving != NULL) {
        if (gateEvaluateOne(g, g->serving, x, ext_score, ext_label, &ev->serving) != GATE_OK)
            goto fail;
        ev->has_serving = 1;
    }
    if (g->candidate != NULL) {
        if (gateEvaluateOne(g, g->candidate, x, ext_score, ext_label, &ev->candidate) != GATE_OK)
            goto fail;
        ev->has_candidate = 1;
    }
    return GATE_OK;

fail:
    gateEvalClear(ev);
    return GATE_ERR;
}

/* What the gate makes of the realized predictions of one request, before
 * the audit bit and the pause flag are taken into account. */
typedef struct gateVerdict {
    int would_accept;     /* inside the serving policy region */
    uint32_t row;         /* region row of the serving prediction */
    gateReason reason;    /* why not, when it is not a would-accept */
    gatePrediction serving, candidate; /* label -1 / score 0 when absent */
} gateVerdict;

/* Copy the prediction of an existing generation out of 'ev'. 0 if 'ev' has
 * none, or one this gate cannot have made. A gate without classes has no
 * labels: whatever the caller put there is replaced by -1. */
static int gateTakePrediction(const gate *g, int has, const gatePrediction *in,
                              gatePrediction *out)
{
    if (!has || !isfinite(in->score) || !gateLabelUsable(g, in->label)) return 0;
    out->label = g->cfg.nclasses != 0 ? in->label : -1;
    out->score = in->score;
    return 1;
}

/* Pure. GATE_ERR when a generation exists for which 'ev' carries no usable
 * prediction; predictions for generations that do not exist are ignored. */
static int gateJudge(const gate *g, const gateEval *ev, gateVerdict *v) {
    const gateGeneration *serv = g->serving;

    v->would_accept = 0;
    v->row = 0;
    v->reason = gateIdleReason(g);
    v->serving.label = v->candidate.label = -1;
    v->serving.score = v->candidate.score = 0.0;

    if (g->candidate != NULL &&
        (ev == NULL || !gateTakePrediction(g, ev->has_candidate, &ev->candidate, &v->candidate)))
        return GATE_ERR;
    if (serv == NULL) return GATE_OK;
    if (ev == NULL || !gateTakePrediction(g, ev->has_serving, &ev->serving, &v->serving))
        return GATE_ERR;

    if (g->cfg.class_mode) v->row = (uint32_t)v->serving.label;
    if (!gateRowTested(&g->cfg, v->row) || serv->policy[v->row] < 0) {
        v->reason = GATE_REASON_CLASS_DEFERRED;
    } else if (v->serving.score >= g->grid[serv->policy[v->row]]) {
        v->would_accept = 1;
        v->reason = GATE_REASON_NONE;
    } else {
        v->reason = GATE_REASON_BELOW_THRESHOLD;
    }
    return GATE_OK;
}

int gatePredictNeedsFeatures(const gate *g, int audit, const gateEval *ev) {
    gateVerdict v;

    if (g->cfg.mode != GATE_MODE_CLASSIFIER || g->recovery_required) return 0;
    /* An evaluation gatePredict() will refuse cannot be a would-accept. */
    if (gateJudge(g, ev, &v) != GATE_OK) return 1;
    return !(v.would_accept && !g->paused && !audit);
}

int gatePredict(gate *g, int64_t now_ms, int audit, const gateEval *ev,
                gateSparse *x, gatePredictResult *res)
{
    int classifier = g->cfg.mode == GATE_MODE_CLASSIFIER;
    int cand_elig, serv_elig, want_obs, action;
    gateReason reason;
    gateVerdict v;
    gateObs proto, *o = NULL;
    uint32_t nnz;

    gateMaintain(g, now_ms);
    audit = audit != 0;

    /* ---- Decide. Nothing is changed until every refusal is behind us. ---- */
    if (res == NULL || gateJudge(g, ev, &v) != GATE_OK) return GATE_ERR;
    reason = v.reason;
    cand_elig = audit && g->candidate != NULL;
    serv_elig = audit && g->serving != NULL &&
                (v.would_accept || (g->cfg.expand && !g->serving->closed));
    if (v.would_accept && !g->paused) {
        action = audit ? GATE_ACTION_AUDIT : GATE_ACTION_ACCEPT;
    } else {
        action = GATE_ACTION_ABSTAIN;
        /* Recovery outranks the pause; gateIdleReason() already said so. */
        if (g->paused && !g->recovery_required) reason = GATE_REASON_PAUSED;
    }
    /* A gate awaiting recovery issues no ids: its epoch is about to change
     * and nothing it learned from them could be trusted. */
    want_obs = action != GATE_ACTION_ACCEPT && !g->recovery_required &&
               (cand_elig || serv_elig || classifier);
    if (want_obs && classifier && !gateSparseUsable(x, g->trainer.dim)) return GATE_ERR;

    /* ---- Commit. ---- */
    g->stats.requests++;
    if (v.would_accept) g->stats.would_accept++;

    if (action == GATE_ACTION_ACCEPT) {
        g->stats.accepted++;
        res->action = GATE_ACTION_ACCEPT;
        res->reason = GATE_REASON_NONE;
        res->label = v.serving.label;
        res->score = v.serving.score;
        res->generation = g->serving->gen;
        res->risk_target = gateRowAlpha(g, v.row);
        res->seq = 0;
        return GATE_OK;
    }

    res->seq = 0;
    if (want_obs) {
        /* Everything the observation records about this request, built once:
         * it becomes either the stored observation or, when that cannot be
         * stored, the stand-in handed to the ONMISSING policy. */
        memset(&proto, 0, sizeof(proto));
        proto.created_ms = g->now_ms;
        proto.result = -1;
        if (audit) proto.flags |= GATE_OBS_AUDIT;
        if (v.would_accept) proto.flags |= GATE_OBS_WOULD_ACCEPT;
        if (cand_elig) proto.flags |= GATE_OBS_CAND;
        if (serv_elig) proto.flags |= GATE_OBS_SERV;
        proto.cand_label = v.candidate.label;
        proto.cand_score = v.candidate.score;
        proto.cand_gen = g->candidate != NULL ? g->candidate->gen : 0;
        proto.serv_label = v.serving.label;
        proto.serv_score = v.serving.score;
        proto.serv_gen = g->serving != NULL ? g->serving->gen : 0;

        nnz = classifier ? x->n : 0;
        if (g->npending < g->cfg.max_pending &&
            g->fixed_bytes + g->pending_bytes + GATE_OBS_BYTES(nnz) <= g->cfg.max_memory)
            o = gateObsAllocate();

        if (o != NULL) {
            *o = proto;
            o->seq = g->next_seq++;
            if (classifier) {
                o->x = *x;
                x->n = 0;
                x->idx = NULL;
                x->val = NULL;
            }
            gateObsLink(g, o);
            g->stats.observations++;
            if (o->flags & GATE_OBS_ELIGIBLE) g->stats.eligible++;
            res->seq = o->seq;
        } else {
            /* Over a limit (or out of memory, which is the same thing to the
             * protocol): the request cannot be recorded, so it cannot be
             * reported as an audit either. If its label was owed to a
             * generation, that debt is settled now. */
            g->stats.resource_limited++;
            if (proto.flags & GATE_OBS_ELIGIBLE) {
                gateMissing(g, &proto);
                gateDrain(g);
            }
            action = GATE_ACTION_ABSTAIN;
            reason = GATE_REASON_RESOURCE_LIMIT;
        }
    }

    res->action = action;
    res->label = -1;
    res->score = 0.0;
    res->risk_target = 0.0;
    /* Read last: settling a missing observation may have revoked serving. */
    res->generation = g->serving != NULL ? g->serving->gen : 0;
    if (action == GATE_ACTION_AUDIT) {
        g->stats.audited++;
        res->reason = GATE_REASON_NONE;
        res->label = v.serving.label;
        res->score = v.serving.score;
    } else {
        g->stats.abstained++;
        if ((unsigned)reason < GATE_REASON_COUNT) g->stats.abstain_reason[reason]++;
        res->reason = reason;
    }
    return GATE_OK;
}

/* -------------------------------- Feedback -------------------------------- */

/* The caller guarantees that 'fb' matches the gate's mode. This is the part
 * of that guarantee memory safety depends on: the label indexes a stack
 * buffer and is stored where the decoder expects a class index. */
static int gateFeedbackUsable(const gate *g, const gateFeedback *fb) {
    if (fb == NULL) return 0;
    if (g->cfg.mode == GATE_MODE_SCORE)
        return fb->kind == GATE_FEEDBACK_CORRECT && (fb->correct == 0 || fb->correct == 1);
    if (fb->label < 0 || (uint32_t)fb->label >= g->cfg.nclasses) return 0;
    if (fb->kind == GATE_FEEDBACK_LABEL) return 1;
    return fb->kind == GATE_FEEDBACK_PROBS && fb->probs != NULL;
}

/* Consume a pending, unresolved observation with its teacher verdict. */
static void gateConsume(gate *g, gateObs *o, const gateFeedback *fb) {
    if (g->cfg.mode == GATE_MODE_CLASSIFIER) {
        double onehot[GATE_MAX_CLASSES];
        const double *q = fb->probs;
        uint32_t k;

        /* The outcome is recorded before the trainer moves. The evidence
         * reads only the predictions frozen in the observation, so the
         * order could not change it; it is kept anyway, as specified. */
        o->result = fb->label;
        if (fb->kind == GATE_FEEDBACK_LABEL) {
            for (k = 0; k < g->cfg.nclasses; k++) onehot[k] = 0.0;
            onehot[fb->label] = 1.0;
            q = onehot;
        }
        if (gateModelLearn(&g->trainer, &o->x, q, g->cfg.lr, g->cfg.l2, NULL) != GATE_OK)
            g->stats.train_rejected++;
        /* The features have served their purpose, even if the observation
         * itself still has to wait for its turn as evidence. */
        g->pending_bytes -= GATE_SPARSE_BYTES(o->x.n);
        gateSparseFree(&o->x);
    } else {
        o->result = fb->correct;
    }

    if (o->flags & GATE_OBS_ELIGIBLE) {
        o->flags |= GATE_OBS_RESOLVED;
        gateDrain(g); /* may release 'o' */
    } else {
        gateObsUnlink(g, o);
        gateObsRelease(o);
    }
}

gateFeedbackStatus gateLearn(gate *g, int64_t now_ms, uint64_t epoch, uint64_t seq,
                             const gateFeedback *fb, const char *teacher)
{
    gateFeedbackStatus status;
    gateObs *o;

    gateMaintain(g, now_ms);

    if (epoch != g->epoch || seq == 0 || seq >= g->next_seq) {
        status = GATE_FB_UNKNOWN;
    } else if ((o = gateFindObservation(g, seq)) == NULL) {
        /* No per-id history is kept for consumed ids, only two watermarks:
         * an id below one of them reports the watermark's status even if it
         * was consumed before the watermark passed it. */
        if (seq < g->invalid_below) status = GATE_FB_INVALIDATED;
        else if (seq <= g->expired_through) status = GATE_FB_EXPIRED;
        else status = GATE_FB_DUPLICATE;
    } else if (o->flags & GATE_OBS_RESOLVED) {
        status = GATE_FB_DUPLICATE;
    } else if (teacher != NULL && strcmp(teacher, g->cfg.teacher) != 0) {
        status = GATE_FB_TEACHER_MISMATCH;
    } else if (!gateFeedbackUsable(g, fb)) {
        /* Not a verdict this gate can take. Like a verdict from the wrong
         * teacher it changes nothing and leaves the observation pending: a
         * proper verdict can still arrive, or the TTL settles it. */
        status = GATE_FB_UNKNOWN;
    } else {
        gateConsume(g, o, fb);
        status = GATE_FB_APPLIED;
    }
    g->stats.feedback[status]++;
    return status;
}

int gateTrain(gate *g, int64_t now_ms, const gateSparse *x, const double *q) {
    gateMaintain(g, now_ms);
    if (g->cfg.mode != GATE_MODE_CLASSIFIER || q == NULL ||
        !gateSparseUsable(x, g->trainer.dim) ||
        gateModelLearn(&g->trainer, x, q, g->cfg.lr, g->cfg.l2, NULL) != GATE_OK)
    {
        g->stats.train_rejected++;
        return GATE_ERR;
    }
    return GATE_OK;
}

/* --------------------------------- Control -------------------------------- */

int gateFreeze(gate *g, int64_t now_ms, uint64_t *gen) {
    int classifier = g->cfg.mode == GATE_MODE_CLASSIFIER;
    gateGeneration *cand;

    gateMaintain(g, now_ms);
    if (g->recovery_required) return GATE_FREEZE_ERR_RECOVERY;
    if (g->candidate != NULL) return GATE_FREEZE_ERR_CANDIDATE;
    if (classifier && g->trainer.updates == 0) return GATE_FREEZE_ERR_UNTRAINED;
    /* A ledger that cannot count one more generation cannot fund it: the
     * index would wrap around and hand out a number, and a budget, that was
     * already spent. 2^64 freezes do not happen; a forged ledger might. */
    if (g->generations == UINT64_MAX) return GATE_FREEZE_ERR_NOMEM;

    /* Everything that can fail comes before the ledger is touched: a
     * generation number is spent if and only if its generation exists. */
    cand = gateGenerationAllocate(g);
    if (cand == NULL) return GATE_FREEZE_ERR_NOMEM;
    if (classifier && gateModelCopy(&cand->model, &g->trainer) != GATE_OK) {
        gateGenerationRelease(cand);
        return GATE_FREEZE_ERR_NOMEM;
    }

    g->generations++;
    cand->gen = g->generations;
    cand->frozen_ms = g->now_ms;
    cand->regions_tested = gateTestedRegions(&g->cfg);
    cand->delta_gen = gateBudgetGenerationDelta(g->cfg.delta, cand->gen);
    cand->delta_region = cand->delta_gen / (double)cand->regions_tested;
    cand->log_threshold = gateBudgetLogThreshold(g->cfg.delta, cand->gen, cand->regions_tested);
    g->candidate = cand;
    if (gen != NULL) *gen = cand->gen;
    return GATE_FREEZE_OK;
}

void gateReset(gate *g, int64_t now_ms, gateResetScope scope) {
    gateMaintain(g, now_ms);
    if (scope != GATE_RESET_CANDIDATE && scope != GATE_RESET_CALIBRATION &&
        scope != GATE_RESET_MODEL && scope != GATE_RESET_ALL)
        return;

    gateDiscardCandidate(g);
    if (scope != GATE_RESET_CANDIDATE) {
        /* The operator withdrew the certification: nothing was found wrong
         * with it, so this is not a revocation and leaves no reason behind. */
        gateGenerationRelease(g->serving);
        g->serving = NULL;
        g->revoked_reason = GATE_REASON_NONE;
    }
    if (scope == GATE_RESET_MODEL || scope == GATE_RESET_ALL) {
        if (g->cfg.mode == GATE_MODE_CLASSIFIER) gateModelReset(&g->trainer);
    }
    if (scope == GATE_RESET_ALL) {
        gateDropPending(g);
        memset(&g->stats, 0, sizeof(g->stats));
        g->last_revoked_reason = GATE_REASON_NONE;
        g->last_revoked_ms = 0;
        g->last_revoked_gen = 0;
    }
    gateDrain(g);
}

void gatePause(gate *g, int64_t now_ms) {
    gateMaintain(g, now_ms);
    g->paused = 1;
}

void gateResume(gate *g, int64_t now_ms) {
    gateMaintain(g, now_ms);
    g->paused = 0;
}

void gateSetAudit(gate *g, int64_t now_ms, double audit) {
    gateMaintain(g, now_ms);
    /* A value outside (0,1] (NaN included) is not a sampling probability:
     * storing it would leave a configuration that no longer validates. */
    if (!(audit > 0.0 && audit <= 1.0) || audit == g->cfg.audit) return;
    g->cfg.audit = audit;
    gateDiscardCandidate(g);
    gateRevoke(g, GATE_REASON_CONFIG_CHANGED);
    gateDrain(g);
}

/* Shared by the teacher and the schema version: both name what the stored
 * features, the weights learned from them and every certification refer to,
 * so a new value invalidates all three. */
static int gateSetVersion(gate *g, int64_t now_ms, char **field, const char *value,
                          gateReason reason)
{
    size_t len;
    uint64_t fixed;
    char *copy;

    gateMaintain(g, now_ms);
    if (value == NULL) return GATE_ERR;
    if (strcmp(*field, value) == 0) return GATE_OK;

    len = strlen(value);
    if (!gateValidName(value, len, GATE_MAX_VERSION_BYTES)) return GATE_ERR;
    /* The version strings are part of the fixed footprint that MAXMEMORY
     * was validated against. A longer one must still fit, or the gate would
     * hold a configuration that gateConfigValidate() (and with it the
     * decoder, on the next reload) rejects. */
    fixed = g->fixed_bytes - (strlen(*field) + 1) + (len + 1);
    if (fixed > g->cfg.max_memory) return GATE_ERR;
    copy = gate_malloc(len + 1);
    if (copy == NULL) return GATE_ERR;
    memcpy(copy, value, len + 1);

    gateDiscardCandidate(g);
    gateRevoke(g, reason);
    if (g->cfg.mode == GATE_MODE_CLASSIFIER) gateModelReset(&g->trainer);
    gateDropPending(g); /* empties the FIFO: nothing is left to drain */
    gate_free(*field);
    *field = copy;
    g->fixed_bytes = gateFixedBytes(&g->cfg);
    return GATE_OK;
}

int gateSetTeacher(gate *g, int64_t now_ms, const char *teacher) {
    return gateSetVersion(g, now_ms, &g->cfg.teacher, teacher, GATE_REASON_TEACHER_CHANGED);
}

int gateSetSchemaVersion(gate *g, int64_t now_ms, const char *version) {
    return gateSetVersion(g, now_ms, &g->cfg.schema_version, version,
                          GATE_REASON_SCHEMA_CHANGED);
}

/* -------------------------------- Recovery -------------------------------- */

void gateFence(gate *g, int64_t now_ms, uint64_t new_epoch) {
    gateMaintain(g, now_ms);
    gateDiscardCandidate(g);
    gateRevoke(g, GATE_REASON_RECOVERY_REQUIRED);
    /* Unconditionally, not only when something was serving: whatever the
     * gate was abstaining for before, it now abstains because its history
     * cannot be vouched for. */
    g->revoked_reason = GATE_REASON_RECOVERY_REQUIRED;
    g->recovery_required = 1;
    gateDropPending(g);
    g->epoch = new_epoch;
}

int gateRecover(gate *g, int64_t now_ms, uint64_t new_epoch) {
    if (!g->recovery_required) return GATE_ERR;
    gateMaintain(g, now_ms);
    g->recovery_required = 0;
    g->revoked_reason = GATE_REASON_NONE;
    if (g->recoveries < UINT32_MAX) g->recoveries++;
    g->epoch = new_epoch;
    g->invalid_below = g->next_seq;
    return GATE_OK;
}

/* ------------------------------- Invariants ------------------------------- */

#define GATE_INVARIANT(cond, ...) do { \
    if (!(cond)) { \
        if (err != NULL && errlen > 0) snprintf(err, errlen, __VA_ARGS__); \
        return 0; \
    } \
} while (0)

#define GATE_OBS_ALL_FLAGS (GATE_OBS_AUDIT | GATE_OBS_WOULD_ACCEPT | GATE_OBS_CAND | \
                            GATE_OBS_SERV | GATE_OBS_RESOLVED | GATE_OBS_IMPUTED)

/* Values fixed at freeze time are compared with a tolerance, not exactly:
 * a gate decoded on another host must not be rejected because its libm
 * rounds a logarithm one ulp differently. */
static int gateNearlyEqual(double a, double b) {
    return fabs(a - b) <= 1e-9 * (fabs(a) > fabs(b) ? fabs(a) : fabs(b));
}

static int gateCheckGeneration(const gate *g, const gateGeneration *gen, int serving,
                               char *err, size_t errlen)
{
    const char *who = serving ? "serving" : "candidate";
    uint32_t T = g->cfg.nthresholds, row, t;
    int certified_rows = 0;
    double delta_gen;

    GATE_INVARIANT(gen->regions != NULL && gen->policy != NULL && gen->drift != NULL,
                   "%s generation has missing arrays", who);
    GATE_INVARIANT(gen->gen >= 1 && gen->gen <= g->generations,
                   "%s generation %llu is outside the ledger (%llu frozen)", who,
                   (unsigned long long)gen->gen, (unsigned long long)g->generations);
    GATE_INVARIANT(gen->regions_tested == gateTestedRegions(&g->cfg),
                   "%s generation tests %u regions, the configuration has %u", who,
                   gen->regions_tested, gateTestedRegions(&g->cfg));
    delta_gen = gateBudgetGenerationDelta(g->cfg.delta, gen->gen);
    GATE_INVARIANT(gateNearlyEqual(gen->delta_gen, delta_gen) &&
                   gateNearlyEqual(gen->delta_region, delta_gen / (double)gen->regions_tested) &&
                   gateNearlyEqual(gen->log_threshold,
                                   gateBudgetLogThreshold(g->cfg.delta, gen->gen,
                                                          gen->regions_tested)),
                   "%s generation budget does not match the ledger", who);
    if (g->cfg.mode == GATE_MODE_CLASSIFIER) {
        GATE_INVARIANT(gen->model.w != NULL && gen->model.b != NULL &&
                       gen->model.dim == gateSchemaDim(&g->cfg.schema) &&
                       gen->model.nclasses == g->cfg.nclasses,
                       "%s generation has no frozen model of the configured shape", who);
    } else {
        GATE_INVARIANT(gen->model.w == NULL && gen->model.b == NULL,
                       "%s generation of a SCORE gate has a model", who);
    }
    GATE_INVARIANT(gen->errors <= gen->samples && gen->imputed <= gen->errors,
                   "%s generation sample counters are inconsistent", who);
    GATE_INVARIANT(gen->closed == 0 || gen->closed == 1, "%s generation: invalid closed flag", who);
    GATE_INVARIANT(gen->frozen_ms <= g->now_ms, "%s generation was frozen in the future", who);

    for (row = 0; row < g->nrows; row++) {
        const gateRegion *r = gen->regions + (size_t)row * T;
        const gateDriftWindow *w = &gen->drift[row];
        int tested = gateRowTested(&g->cfg, row);
        int32_t lowest = -1;

        for (t = 0; t < T; t++) {
            GATE_INVARIANT(r[t].errors <= r[t].n && r[t].certified_at <= r[t].n &&
                           r[t].n <= gen->samples,
                           "%s region %u/%u counters are inconsistent", who, row, t);
            /* Nested regions: a sample in region t is in every lower one. */
            GATE_INVARIANT(t == 0 || (r[t].n <= r[t - 1].n && r[t].errors <= r[t - 1].errors),
                           "%s regions %u/%u and %u/%u are not nested", who, row, t - 1, row, t);
            GATE_INVARIANT(tested || (r[t].n == 0 && r[t].certified_at == 0),
                           "%s region %u/%u of an ALWAYSDEFER class has samples", who, row, t);
            if (lowest < 0 && r[t].certified_at != 0) lowest = (int32_t)t;
        }
        GATE_INVARIANT(gen->policy[row] == lowest,
                       "%s policy of row %u is %d, lowest certified threshold is %d", who, row,
                       (int)gen->policy[row], (int)lowest);
        if (lowest >= 0) certified_rows++;

        GATE_INVARIANT(w->n <= GATE_DRIFT_WINDOW && w->pos < GATE_DRIFT_WINDOW &&
                       w->errors <= w->n && w->n <= w->total && w->total_errors <= w->total,
                       "%s drift window of row %u is inconsistent", who, row);
        if (serving) {
            double alpha = gateRowAlpha(g, row);
            double margin = g->cfg.drift_margin < 0 ? alpha : g->cfg.drift_margin;

            /* A window that trips the alarm revokes at once, so it is never
             * found on a generation that is still serving. */
            GATE_INVARIANT(w->n < GATE_DRIFT_MIN ||
                           (double)w->errors <= (alpha + margin) * (double)w->n,
                           "serving row %u is past its drift limit", row);
        } else {
            GATE_INVARIANT(w->total == 0, "candidate row %u has audits", row);
        }
    }

    if (serving) {
        GATE_INVARIANT(gen->promoted_ms > 0, "serving generation has no promotion time");
        GATE_INVARIANT(gen->frozen_ms <= gen->promoted_ms && gen->promoted_ms <= g->now_ms,
                       "serving generation timestamps are out of order");
        GATE_INVARIANT(gen->expires_ms == gateDeadline(gen->promoted_ms, g->cfg.cert_ttl_ms),
                       "serving generation expiry is not promotion + CERTTTL");
        GATE_INVARIANT(certified_rows > 0, "serving generation has no certified region");
        GATE_INVARIANT(gen->policy_updates >= (uint32_t)certified_rows,
                       "serving generation has fewer policy updates than certified rows");
        GATE_INVARIANT(g->cfg.expand || gen->closed,
                       "serving generation is still testing with EXPAND off");
    } else {
        GATE_INVARIANT(gen->promoted_ms == 0 && gen->expires_ms == 0,
                       "candidate generation has promotion timestamps");
        GATE_INVARIANT(certified_rows == 0 && gen->policy_updates == 0,
                       "candidate generation has a certified region but was not promoted");
        GATE_INVARIANT(!gen->closed, "candidate generation is closed");
    }
    return 1;
}

int gateCheckInvariants(const gate *g, char *err, size_t errlen) {
    const gateConfig *cfg = &g->cfg;
    int classifier = cfg->mode == GATE_MODE_CLASSIFIER;
    int64_t ttl = cfg->observation_ttl_ms;
    uint32_t count = 0, flagged = 0, queued = 0, indexed = 0, slot, row, t;
    uint64_t bytes = 0, reasons = 0;
    const gateObs *o, *prev;

    if (err != NULL && errlen > 0) err[0] = '\0';

    /* ---- Shape. ---- */
    if (gateConfigValidate(cfg, err, errlen) != GATE_OK) return 0;
    GATE_INVARIANT(g->grid != NULL && g->bets != NULL && g->table != NULL,
                   "gate has missing arrays");
    GATE_INVARIANT(g->nrows == gateConfigRows(cfg), "nrows does not match the configuration");
    GATE_INVARIANT(g->table_size == gateTableSize(cfg->max_pending),
                   "index has %u buckets, MAXPENDING needs %u", g->table_size,
                   gateTableSize(cfg->max_pending));
    for (t = 0; t < cfg->nthresholds; t++)
        GATE_INVARIANT(isfinite(g->grid[t]) && (t == 0 || g->grid[t] > g->grid[t - 1]),
                       "threshold grid is not strictly increasing at %u", t);
    for (row = 0; row < g->nrows; row++)
        GATE_INVARIANT(g->bets[row].alpha == gateRowAlpha(g, row),
                       "bets of row %u were not built for its alpha", row);
    if (classifier) {
        GATE_INVARIANT(g->trainer.w != NULL && g->trainer.b != NULL &&
                       g->trainer.dim == gateSchemaDim(&cfg->schema) &&
                       g->trainer.nclasses == cfg->nclasses,
                       "trainer does not have the configured shape");
    } else {
        GATE_INVARIANT(g->trainer.w == NULL && g->trainer.b == NULL && g->trainer.updates == 0,
                       "SCORE gate has a trainer");
    }

    /* ---- Lifecycle. ---- */
    GATE_INVARIANT((g->paused == 0 || g->paused == 1) &&
                   (g->recovery_required == 0 || g->recovery_required == 1),
                   "invalid pause / recovery flag");
    GATE_INVARIANT(g->created_ms <= g->now_ms, "gate was created in the future");
    GATE_INVARIANT((unsigned)g->revoked_reason < GATE_REASON_COUNT &&
                   (unsigned)g->last_revoked_reason < GATE_REASON_COUNT,
                   "invalid revocation reason");
    GATE_INVARIANT(g->serving == NULL || g->revoked_reason == GATE_REASON_NONE,
                   "a generation is serving but a revocation reason is set");
    GATE_INVARIANT((g->last_revoked_reason == GATE_REASON_NONE) == (g->last_revoked_gen == 0) &&
                   g->last_revoked_gen <= g->generations && g->last_revoked_ms <= g->now_ms,
                   "last revocation record is inconsistent");
    GATE_INVARIANT(!g->recovery_required ||
                   (g->candidate == NULL && g->serving == NULL && g->npending == 0),
                   "gate awaiting recovery still holds generations or observations");
    GATE_INVARIANT(g->candidate == NULL || g->serving == NULL ||
                   g->candidate->gen != g->serving->gen,
                   "candidate and serving share generation %llu",
                   (unsigned long long)(g->candidate ? g->candidate->gen : 0));
    if (g->candidate != NULL && !gateCheckGeneration(g, g->candidate, 0, err, errlen)) return 0;
    if (g->serving != NULL && !gateCheckGeneration(g, g->serving, 1, err, errlen)) return 0;

    /* ---- Observation ids. ---- */
    GATE_INVARIANT(g->next_seq >= 1 && g->expired_through < g->next_seq &&
                   g->invalid_below <= g->next_seq,
                   "id watermarks are ahead of the next sequence number");

    /* ---- Index. Walked first and bounded by npending, so that a corrupted
     * chain cannot make the lookups below spin. ---- */
    for (slot = 0; slot < g->table_size; slot++) {
        for (o = g->table[slot]; o != NULL; o = o->hnext) {
            GATE_INVARIANT(indexed < g->npending,
                           "index holds more entries than there are pending observations");
            GATE_INVARIANT(gateTableSlot(o->seq, g->table_size) == slot,
                           "observation %llu is indexed in the wrong bucket",
                           (unsigned long long)o->seq);
            indexed++;
        }
    }
    GATE_INVARIANT(indexed == g->npending, "index holds %u entries, %u observations are pending",
                   indexed, g->npending);

    /* ---- Pending list. Every member is found through the index, and the
     * index holds exactly npending entries: the two are the same set. ---- */
    for (o = g->oldest, prev = NULL; o != NULL; prev = o, o = o->next) {
        unsigned long long seq = (unsigned long long)o->seq;
        int resolved = (o->flags & GATE_OBS_RESOLVED) != 0;

        GATE_INVARIANT(count < g->npending, "pending list is longer than npending (%u)",
                       g->npending);
        count++;
        GATE_INVARIANT(o->prev == prev, "observation %llu has a broken back link", seq);
        GATE_INVARIANT(o->seq >= 1 && o->seq < g->next_seq,
                       "observation %llu is outside the issued ids", seq);
        GATE_INVARIANT(prev == NULL || prev->seq < o->seq,
                       "pending list is not in ascending seq order at %llu", seq);
        GATE_INVARIANT(prev == NULL || prev->created_ms <= o->created_ms,
                       "pending list is not in creation order at %llu", seq);
        GATE_INVARIANT(o->created_ms <= g->now_ms, "observation %llu is from the future", seq);
        GATE_INVARIANT(gateDeadline(o->created_ms, ttl) > g->now_ms,
                       "observation %llu outlived its TTL but is still pending", seq);
        GATE_INVARIANT(o->seq >= g->invalid_below && o->seq > g->expired_through,
                       "observation %llu is pending below an id watermark", seq);
        GATE_INVARIANT(gateFindObservation(g, o->seq) == o,
                       "observation %llu is not reachable through the index", seq);

        GATE_INVARIANT((o->flags & ~(uint32_t)GATE_OBS_ALL_FLAGS) == 0,
                       "observation %llu has unknown flags", seq);
        /* Imputation resolves, applies and releases in one step. */
        GATE_INVARIANT(!(o->flags & GATE_OBS_IMPUTED),
                       "observation %llu is imputed but still pending", seq);
        GATE_INVARIANT(!(o->flags & GATE_OBS_ELIGIBLE) || (o->flags & GATE_OBS_AUDIT),
                       "observation %llu is eligible without an audit bit", seq);
        GATE_INVARIANT(!resolved || (o->flags & GATE_OBS_ELIGIBLE),
                       "observation %llu is resolved but not queued as evidence", seq);
        GATE_INVARIANT(!(o->flags & GATE_OBS_CAND) || o->cand_gen != 0,
                       "observation %llu is evidence for a candidate it does not name", seq);
        GATE_INVARIANT(!(o->flags & (GATE_OBS_SERV | GATE_OBS_WOULD_ACCEPT)) || o->serv_gen != 0,
                       "observation %llu refers to a serving generation it does not name", seq);
        GATE_INVARIANT(o->cand_gen <= g->generations && o->serv_gen <= g->generations &&
                       (o->cand_gen == 0 || o->cand_gen != o->serv_gen),
                       "observation %llu names impossible generations", seq);
        GATE_INVARIANT(isfinite(o->cand_score) && isfinite(o->serv_score),
                       "observation %llu stores a non-finite score", seq);
        if (cfg->nclasses == 0) {
            GATE_INVARIANT(o->cand_label == -1 && o->serv_label == -1,
                           "observation %llu has a label but the gate has no classes", seq);
        } else {
            GATE_INVARIANT(o->cand_gen != 0 ? gateLabelUsable(g, o->cand_label)
                                            : o->cand_label == -1,
                           "observation %llu has an invalid candidate label", seq);
            GATE_INVARIANT(o->serv_gen != 0 ? gateLabelUsable(g, o->serv_label)
                                            : o->serv_label == -1,
                           "observation %llu has an invalid serving label", seq);
        }
        if (!resolved) {
            GATE_INVARIANT(o->result == -1, "observation %llu has a result but is unresolved", seq);
        } else if (classifier) {
            GATE_INVARIANT(o->result >= 0 && (uint32_t)o->result < cfg->nclasses,
                           "observation %llu resolved with an invalid class", seq);
        } else {
            GATE_INVARIANT(o->result == 0 || o->result == 1,
                           "observation %llu resolved with an invalid CORRECT bit", seq);
        }
        if (classifier && !resolved) {
            GATE_INVARIANT(gateSparseUsable(&o->x, g->trainer.dim),
                           "observation %llu stores unusable features", seq);
        } else {
            GATE_INVARIANT(o->x.n == 0 && o->x.idx == NULL && o->x.val == NULL,
                           "observation %llu keeps features it has no use for", seq);
        }

        bytes += GATE_OBS_BYTES(o->x.n);
        if (o->flags & GATE_OBS_ELIGIBLE) flagged++;
    }
    GATE_INVARIANT(count == g->npending && prev == g->newest,
                   "pending list has %u members, npending is %u", count, g->npending);
    GATE_INVARIANT(bytes == g->pending_bytes, "pending_bytes is %llu, observations add up to %llu",
                   (unsigned long long)g->pending_bytes, (unsigned long long)bytes);

    /* ---- Eligible FIFO: exactly the flagged observations, in order. ---- */
    for (o = g->ehead, prev = NULL; o != NULL; prev = o, o = o->enext) {
        GATE_INVARIANT(queued < g->neligible, "eligible FIFO is longer than neligible (%u)",
                       g->neligible);
        queued++;
        GATE_INVARIANT(o->flags & GATE_OBS_ELIGIBLE,
                       "observation %llu is queued without an eligibility flag",
                       (unsigned long long)o->seq);
        GATE_INVARIANT(prev == NULL || prev->seq < o->seq,
                       "eligible FIFO is not in ascending seq order at %llu",
                       (unsigned long long)o->seq);
        GATE_INVARIANT(gateFindObservation(g, o->seq) == o,
                       "queued observation %llu is not pending", (unsigned long long)o->seq);
    }
    GATE_INVARIANT(queued == g->neligible && prev == g->etail,
                   "eligible FIFO has %u members, neligible is %u", queued, g->neligible);
    GATE_INVARIANT(queued == flagged,
                   "%u observations are flagged eligible, %u are queued", flagged, queued);
    GATE_INVARIANT(g->ehead == NULL ||
                   (!(g->ehead->flags & GATE_OBS_RESOLVED) && gateObsLive(g, g->ehead)),
                   "head of the eligible FIFO (%llu) should have been drained",
                   (unsigned long long)(g->ehead ? g->ehead->seq : 0));

    /* ---- Memory. ---- */
    GATE_INVARIANT(g->fixed_bytes == gateFixedBytes(cfg), "cached fixed footprint is stale");
    GATE_INVARIANT(gateMemoryUsage(g) <= g->fixed_bytes + g->pending_bytes,
                   "memory usage %llu exceeds the fixed footprint plus pending bytes %llu",
                   (unsigned long long)gateMemoryUsage(g),
                   (unsigned long long)(g->fixed_bytes + g->pending_bytes));
    GATE_INVARIANT(g->fixed_bytes + g->pending_bytes <= cfg->max_memory,
                   "accounted bytes exceed MAXMEMORY");
    GATE_INVARIANT(g->npending <= cfg->max_pending, "more pending observations than MAXPENDING");

    /* ---- Counters. ---- */
    GATE_INVARIANT(g->stats.requests ==
                   g->stats.accepted + g->stats.audited + g->stats.abstained,
                   "requests != accepted + audited + abstained");
    for (t = 0; t < GATE_REASON_COUNT; t++) reasons += g->stats.abstain_reason[t];
    GATE_INVARIANT(reasons == g->stats.abstained && g->stats.abstain_reason[GATE_REASON_NONE] == 0,
                   "abstain reasons do not add up to the abstentions");
    GATE_INVARIANT(g->stats.accepted + g->stats.audited <= g->stats.would_accept &&
                   g->stats.would_accept <= g->stats.requests,
                   "would_accept is inconsistent with the outcomes");
    GATE_INVARIANT(g->stats.eligible <= g->stats.observations &&
                   g->stats.observations <= g->stats.requests &&
                   g->stats.observations < g->next_seq,
                   "observation counters are inconsistent");
    GATE_INVARIANT(g->stats.expired_eligible <= g->stats.expired &&
                   g->stats.expired <= g->stats.observations,
                   "expiry counters are inconsistent");
    return 1;
}
