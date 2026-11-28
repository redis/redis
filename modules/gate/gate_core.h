/*
 * Gate data type: the gate itself.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Everything a gate does except talking to Redis: snapshots, proof, the
 * feedback protocol, promotion, drift, reset and recovery. SPEC.md section 3
 * describes the behaviour in plain words; this file is its exact form.
 *
 * DETERMINISM. Every mutating function is a deterministic function of the
 * gate and its arguments. The clock ('now_ms'), the random draw ('u'), the
 * random ids ('lineage', 'epoch') and the model outputs ('gateEval') are
 * arguments. The Redis module realizes them once on the primary and
 * replicates the realized values, so a replica or an AOF reload reaches the
 * same state by calling the same functions. Nothing here reads a clock or a
 * random source.
 *
 * ROWS. A classifier gate has one row per label: each label is proven and
 * answered on its own. A score gate has a single row.
 *
 * SNAPSHOTS ("generations"). A generation is a frozen copy of the model
 * (classifier) or simply a round of testing (score), numbered 1, 2, 3, ...
 * per key and never reused. It starts as the CANDIDATE and may become the
 * SERVING generation. It owns one betting test per (row, threshold).
 *
 *   When a snapshot is taken (gateSettle):
 *     classifier  no candidate, and the trainer has learned at least
 *                 snapshot_examples examples since the previous snapshot
 *     score       no candidate and nothing serving
 *
 * THE DRAW. Every request carries one uniform number u in [0,1), drawn
 * before anything is decided. "audit" means u < cfg.audit.
 *
 * DECISION. A request is a WOULD-ACCEPT when a generation is serving, its
 * predicted row has a proven threshold, and score >= that threshold.
 *   ACCEPT   would-accept, not audit, not paused
 *   AUDIT    would-accept, audit, not paused
 *   ABSTAIN  everything else
 *
 * WHICH REQUESTS ARE PROOF. A request is proof for
 *   the serving generation   if its row is tested and either the row has no
 *                            proven threshold yet (every such request goes
 *                            to the teacher, so every one counts) or audit;
 *   the candidate            if nothing is serving (every request), else
 *                            if audit.
 * A request that is proof for something must get its teacher answer: it is
 * EVIDENCE. Evidence is applied strictly in request order; an answer waits
 * until every older piece of evidence is answered or has timed out, so the
 * time a teacher takes cannot select the proof.
 *
 * A piece of evidence stores the guess each generation made when the
 * request was asked. Its error indicator feeds every region of the guessed
 * row whose threshold is <= the stored score. Guesses are never recomputed.
 *
 * MISSING. Evidence whose answer does not arrive within the feedback
 * timeout is applied as an error. That is the worst case for the gate, so
 * it can only make the gate more cautious.
 *
 * PROVEN. A region has CROSSED once its log wealth reached the generation's
 * bar. It is PROVEN if it crossed and it is an entry point or the next
 * more confident region of the row is proven. Classifier rows have entry
 * points at every eighth threshold counting from the most confident; in a
 * score gate every threshold is an entry point. The policy of a row is its
 * lowest proven threshold. Testing never stops, so the policy only moves
 * down.
 *
 *     bar = log(g (g + 1)) + log(tested rows * entry points) - log(delta)
 *
 * PROMOTION. A candidate with at least one proven region is promoted when
 * nothing is serving, or when it accepts more of its own proof requests
 * than the serving generation did (compared on the same requests). A
 * classifier candidate that has consumed its budget of proof without being
 * promoted is dropped.
 *
 * DRIFT. Each serving row runs a drift detector over its audits. An alarm
 * drops the serving generation and the candidate and (classifier) wipes the
 * trainer: the gate starts over.
 *
 * FULL. A gate that already holds GATE_MAX_PENDING waiting requests (or
 * GATE_MAX_PENDING_BYTES of stored clues) answers ABSTAIN to everything
 * without drawing or recording anything. Requests that would only be used
 * for training are no longer recorded above GATE_TRAIN_PENDING.
 *
 * TIME. 'now_ms' is wall-clock milliseconds. The gate never moves
 * backwards: each call uses max(now_ms, the largest value seen so far).
 */

#ifndef GATE_CORE_H
#define GATE_CORE_H

#include <stddef.h>
#include <stdint.h>
#include "gate_defs.h"
#include "gate_stats.h"
#include "gate_model.h"

/* ------------------------------ Enumerations ----------------------------- */

#define GATE_KIND_CLASSIFIER 0
#define GATE_KIND_SCORE 1

typedef enum gateStateId {
    GATE_STATE_COLD = 0,    /* nothing is being tested */
    GATE_STATE_CALIBRATING, /* a candidate is being tested, nothing answers */
    GATE_STATE_ACTIVE,      /* a generation answers */
    GATE_STATE_PAUSED       /* answering switched off by GPAUSE */
} gateStateId;

#define GATE_ACTION_ACCEPT 0
#define GATE_ACTION_ABSTAIN 1
#define GATE_ACTION_AUDIT 2

/* "COLD", "ACCEPT", ... Never NULL. */
const char *gateStateName(gateStateId s);
const char *gateActionName(int action);

/* ----------------------------- Configuration ----------------------------- */

typedef struct gateConfig {
    int kind;                     /* GATE_KIND_* */
    double alpha;                 /* tolerated error rate, (0,1) */
    double delta;                 /* lifetime budget, (0,1) */
    double audit;                 /* spot-check probability, (0,1] */
    uint32_t nclasses;            /* K (classifier); 0 for a score gate */
    char **labels;                /* K NUL-terminated labels */
    double *class_alpha;          /* K error rates, each in (0, alpha] */
    uint8_t *class_defer;         /* K flags: 1 = never answer this label */
    uint32_t snapshot_examples;   /* examples between snapshots, >= 1 */
    int64_t feedback_timeout_ms;  /* how long evidence waits, >= 1 */
} gateConfig;

/* Defaults: score gate, alpha 0 (must be set), DELTA / AUDIT defaults, no
 * classes, the default snapshot step and feedback timeout. */
void gateConfigInit(gateConfig *cfg);
/* Release every heap field of 'cfg' (not 'cfg' itself) and zero it. */
void gateConfigFree(gateConfig *cfg);
/* Deep copy. GATE_OK / GATE_ERR. */
int gateConfigCopy(gateConfig *dst, const gateConfig *src);
/* Give a classifier config its K labels (copied) and allocate class_alpha
 * (all = cfg->alpha at the time of the call... see gateConfigValidate) and
 * class_defer (all 0). GATE_OK / GATE_ERR. */
int gateConfigSetClasses(gateConfig *cfg, const char *const *labels,
                         const size_t *lens, uint32_t n);
/* Check every rule of SPEC.md section 2 (ranges, 2..64 distinct valid
 * labels for a classifier and none for a score gate, class_alpha in
 * (0, alpha], not every label deferred, limits). A class_alpha entry of 0
 * means "not set" and is replaced by alpha before checking, which is why
 * 'cfg' is not const. On failure writes a message into 'err' and returns
 * GATE_ERR. */
int gateConfigValidate(gateConfig *cfg, char *err, size_t errlen);
/* Index of a label, or -1. */
int gateConfigLabelIndex(const gateConfig *cfg, const char *label, size_t len);

/* ------------------------------ Generations ------------------------------ */

typedef struct gateGeneration {
    uint64_t gen;            /* generation number, >= 1 */
    int64_t frozen_ms;
    int64_t promoted_ms;     /* 0 while it is the candidate */
    double log_threshold;    /* the bar */
    gateModel model;         /* frozen weights (classifier); zeroed otherwise */
    gateRegion *regions;     /* nrows * GATE_THRESHOLDS, index row * T + t;
                              * region.certified_at records when it CROSSED */
    int32_t *policy;         /* nrows: lowest proven threshold index, -1 none */
    gateDrift *drift;        /* nrows: drift detectors (used while serving) */
    uint64_t samples;        /* evidence applied to this generation */
    uint64_t errors;         /* ... of which were wrong guesses */
    uint64_t paired;         /* ... which the then-serving generation would
                              * have accepted (candidate only) */
    uint64_t audits;         /* audits applied while serving */
    uint64_t audit_errors;   /* ... of which were wrong guesses */
    uint64_t budget;         /* candidate: dropped once samples reach this
                              * without promotion; 0 = no limit */
} gateGeneration;

/* ------------------------------ Observations ----------------------------- */

#define GATE_OBS_AUDIT (1u << 0)        /* u < audit */
#define GATE_OBS_WOULD_ACCEPT (1u << 1) /* serving would have accepted */
#define GATE_OBS_CAND (1u << 2)         /* proof for generation cand_gen */
#define GATE_OBS_SERV (1u << 3)         /* proof for generation serv_gen */
#define GATE_OBS_RESOLVED (1u << 4)     /* answered, waiting for its turn */
#define GATE_OBS_MISSING (1u << 5)      /* timed out: applied as an error */
#define GATE_OBS_EVIDENCE (GATE_OBS_CAND | GATE_OBS_SERV)

typedef struct gateObs {
    uint64_t seq;        /* request number, >= 1 */
    int64_t created_ms;
    uint32_t flags;      /* GATE_OBS_* */
    int32_t cand_label;  /* frozen guesses: label index, -1 if none */
    int32_t serv_label;
    double cand_score;
    double serv_score;
    uint64_t cand_gen;   /* 0 = no candidate when asked */
    uint64_t serv_gen;   /* 0 = nothing serving when asked */
    int32_t result;      /* answer: label index (classifier) or 0/1 correct
                          * (score); -1 while unanswered */
    gateSparse x;        /* clues kept for training; freed once trained */
    struct gateObs *prev, *next; /* every waiting request, ascending seq */
    struct gateObs *enext;       /* evidence queue, ascending seq */
    struct gateObs *hnext;       /* index chain */
} gateObs;

/* Bytes attributed to one waiting request storing 'nnz' clues. */
#define GATE_OBS_BYTES(nnz) (sizeof(gateObs) + GATE_SPARSE_BYTES(nnz))
/* Buckets of the seq -> request index (a power of two). */
#define GATE_TABLE_SIZE 4096u

/* --------------------------------- Gate ---------------------------------- */

typedef struct gateCounters {
    /* Kept by the primary only: an ACCEPT is not replicated, so replicas
     * do not see these move. Excluded from the digest. */
    uint64_t requests;     /* GPREDICT calls */
    uint64_t accepted;     /* ACCEPT replies */
    uint64_t audited;      /* AUDIT replies */
    uint64_t abstained;    /* ABSTAIN replies */
    uint64_t full;         /* ... of which because the gate was full */
    uint64_t window;       /* requests seen by the serving generation, capped */
    double coverage;       /* running share of them inside its policy */
    /* Replicated. */
    uint64_t observations; /* ids handed out */
    uint64_t learned;      /* feedback used (GLEARN replied 1) */
    uint64_t ignored;      /* feedback not used (GLEARN replied 0) */
    uint64_t missing;      /* evidence that timed out */
    uint64_t dropped;      /* training-only requests that timed out */
    uint64_t promotions;
    uint64_t revocations;  /* drift alarms */
    uint64_t retired;      /* candidates dropped at their budget */
} gateCounters;

typedef struct gate {
    gateConfig cfg;
    uint32_t nrows;                 /* K (classifier) or 1 (score) */
    double grid[GATE_THRESHOLDS];
    gateBets *bets;                 /* nrows: betting increments */
    gateDriftBets *drift_bets;      /* nrows: drift increments */

    uint64_t lineage;        /* random id of this key's budget ledger */
    uint64_t epoch;          /* random part of observation ids */
    int64_t created_ms;
    int64_t now_ms;          /* largest clock value applied (primary-local) */

    uint64_t generations;    /* generations ever frozen: the ledger */
    gateModel trainer;       /* live model (classifier); zeroed otherwise */
    uint64_t snapshot_updates; /* trainer.updates at the last snapshot */
    gateGeneration *candidate;
    gateGeneration *serving;
    int paused;

    uint64_t next_seq;       /* next request number, starts at 1 */
    uint32_t npending;
    uint32_t nevidence;      /* requests in the evidence queue */
    gateObs *oldest, *newest;
    gateObs *ehead, *etail;
    gateObs **table;         /* GATE_TABLE_SIZE chains */
    uint64_t pending_bytes;

    gateCounters stats;

    uint64_t runtime_tag;    /* owned by the embedding layer: never encoded,
                              * never touched here, 0 after decoding */
} gate;

/* ------------------------------- Lifecycle ------------------------------- */

/* Create a gate from a VALIDATED config (deep-copied). 'lineage' and
 * 'epoch' are caller-supplied random numbers. A score gate starts its first
 * generation at once. NULL on allocation failure. */
gate *gateCreate(const gateConfig *cfg, uint64_t lineage, uint64_t epoch, int64_t now_ms);
void gateFree(gate *g);

/* Bytes attributed to the gate: a deterministic function of its state. */
uint64_t gateMemoryUsage(const gate *g);
/* Rough number of heap objects, for lazy-free decisions. */
uint64_t gateFreeEffort(const gate *g);

/* PAUSED if paused, else ACTIVE if a generation serves, else CALIBRATING
 * if there is a candidate, else COLD. */
gateStateId gateState(const gate *g);

/* 1 if gateMaintain(g, now_ms) would change anything besides the clock: a
 * waiting request has timed out, or a snapshot is due. Pure. */
int gateMaintenanceDue(const gate *g, int64_t now_ms);

/* Advance the clock, time out waiting requests (oldest first: unanswered
 * evidence is applied as an error, training-only requests are dropped),
 * apply evidence that this unblocks, take a snapshot if one is due. Every
 * mutating function calls this first. */
void gateMaintain(gate *g, int64_t now_ms);

/* 1 if the gate cannot record another request (see FULL above). While
 * full, gatePredict() abstains without looking at the request. */
int gateFull(const gate *g);

/* ------------------------------- Prediction ------------------------------ */

typedef struct gatePrediction {
    int32_t label; /* label index; -1 for a score gate */
    double score;
} gatePrediction;

typedef struct gateEval {
    int has_serving, has_candidate;
    gatePrediction serving, candidate;
} gateEval;

/* Pure: what the serving and candidate generations guess for a request.
 * Classifier: 'x' are the request's clues, 'ext_score' is ignored.
 * Score: 'x' is NULL and both "guess" (-1, ext_score).
 * GATE_ERR if a model produced a non-finite value. */
int gateEvaluate(const gate *g, const gateSparse *x, double ext_score, gateEval *ev);

typedef struct gatePredictResult {
    int action;     /* GATE_ACTION_* */
    int32_t label;  /* ACCEPT: the answer's label index (-1 for a score gate) */
    uint64_t seq;   /* request recorded for feedback, 0 = none */
} gatePredictResult;

/* 1 if gatePredict() with these arguments would record a request together
 * with its clues (classifier only). Call after gateMaintain(). Lets the
 * caller skip replicating the input of requests that are not recorded. */
int gatePredictNeedsFeatures(const gate *g, double u, const gateEval *ev);

/* Register one request. 'u' is the realized draw in [0,1), 'ev' the
 * realized guesses. See the file comment for the decision and for which
 * requests are recorded:
 *   - full gate: ABSTAIN, seq 0, nothing else happens;
 *   - ACCEPT: nothing is recorded;
 *   - otherwise the request is recorded if it is evidence, or (classifier)
 *     for training while fewer than GATE_TRAIN_PENDING requests wait.
 * 'x' (classifier): when the request is recorded the gate takes ownership
 * of x's arrays and resets *x; otherwise x is untouched. If clues are
 * needed and x is NULL, returns GATE_ERR having done only maintenance
 * (possible only through an inconsistent replication stream). */
int gatePredict(gate *g, int64_t now_ms, double u, const gateEval *ev,
                gateSparse *x, gatePredictResult *res);

/* ------------------------------- Feedback -------------------------------- */

/* The teacher's answer for request ('epoch', 'seq'): 'result' is the label
 * index (classifier) or 0/1 = cheap result wrong/right (score); the caller
 * has validated it. Returns 1 if it was used, 0 if the id is not waiting
 * (wrong epoch, never issued, timed out, already answered). When used: the
 * trainer learns from it (classifier), and if the request is evidence it is
 * applied when its turn comes. */
int gateLearn(gate *g, int64_t now_ms, uint64_t epoch, uint64_t seq, int32_t result);

/* GADD: one training step on a labelled example. Never proof. Classifier
 * only. GATE_OK, or GATE_ERR if the update was rejected as non-finite. */
int gateTrain(gate *g, int64_t now_ms, const gateSparse *x, int32_t label);

/* ------------------------------ Administration --------------------------- */

void gatePause(gate *g, int64_t now_ms);
void gateResume(gate *g, int64_t now_ms);

/* GRESET: forget the model, both generations, every waiting request and
 * the counters; switch to 'new_epoch' so that ids issued before are
 * refused. Keeps the configuration, the pause flag, the lineage and the
 * ledger (generation numbers are not reused, budget is not handed back). */
void gateReset(gate *g, int64_t now_ms, uint64_t new_epoch);

/* After a restart, a failover or a RESTORE: drop both generations and
 * every waiting request and switch to 'new_epoch'. Keeps the trainer, the
 * counters and the ledger. A classifier with enough learned examples takes
 * a fresh snapshot at once. */
void gateRecover(gate *g, int64_t now_ms, uint64_t new_epoch);

/* ------------------------------ Introspection ---------------------------- */

/* Error rate of a row: class_alpha[row] (classifier) or alpha (score). */
double gateRowAlpha(const gate *g, uint32_t row);
/* 1 if the row is tested (not ALWAYSDEFER). */
int gateRowTested(const gate *g, uint32_t row);
/* 1 if threshold index t is an entry point for this kind of gate. */
int gateIsEntry(const gate *g, uint32_t t);
/* Threshold the gate currently answers at for a row, or a negative number
 * if it does not answer for that row. */
double gateRowThreshold(const gate *g, uint32_t row);
/* Share of recent requests that fell inside the serving policy: a running
 * average over about the last GATE_COVERAGE_WINDOW requests, counted when
 * they are asked (so it needs no feedback and has no sampling bias). 0 if
 * nothing serves. A measurement kept by the primary, not a bound. */
#define GATE_COVERAGE_WINDOW 1024
double gateCoverage(const gate *g);
/* DELTA already spent: delta * g / (g + 1). */
double gateBudgetSpentNow(const gate *g);
/* Look up a waiting request (NULL if not waiting). */
gateObs *gateFindObservation(const gate *g, uint64_t seq);
/* Consistency check for tests: lists, index, counters, byte accounting,
 * queue order, policy vs crossed regions. 1 if consistent, else 0 with a
 * message in 'err'. */
int gateCheckInvariants(const gate *g, char *err, size_t errlen);

/* ------------------- Constructors shared with the codec ------------------ */

/* A gate holding a deep copy of the VALIDATED 'cfg' with grid, increments,
 * (classifier) a zeroed trainer and the empty index built, next_seq = 1 and
 * everything else zero. NULL on allocation failure. */
gate *gateAllocate(const gateConfig *cfg);
/* An empty generation shaped for 'g': regions initialized, policy -1,
 * drift detectors initialized, model zeroed (not allocated). */
gateGeneration *gateGenerationAllocate(const gate *g);
void gateGenerationRelease(gateGeneration *gen);
/* A zeroed request with result = cand_label = serv_label = -1. */
gateObs *gateObsAllocate(void);
void gateObsRelease(gateObs *o);
/* Append a request whose seq is larger than every waiting seq: index,
 * list, evidence queue if it has an evidence flag, counts and bytes. */
void gateObsLink(gate *g, gateObs *o);
/* Number of tested rows and of entry points per row. */
uint32_t gateTestedRows(const gate *g);
uint32_t gateEntryPoints(const gate *g);
/* Recompute policy[row] of a generation from its crossed regions. */
void gateGenerationRefreshPolicy(const gate *g, gateGeneration *gen, uint32_t row);

#endif /* GATE_CORE_H */
