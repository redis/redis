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
 *   the candidate            if its row is tested and nothing is serving
 *                            (every such request), else if audit.
 * "Its row" is the label that generation guessed; a guess of an ALWAYSDEFER
 * label proves nothing and is not counted anywhere.
 * A request that is proof for something must get its teacher answer: it is
 * EVIDENCE. Evidence is applied in request order, so the time a teacher
 * takes cannot select the proof: an answer waits for the answers of older
 * requests. It does not wait long. Once an unanswered request is older
 * than the GRACE period (a quarter of the feedback timeout) and holds up
 * answered ones, or holds up more than GATE_MAX_WAITING of them, proof
 * moves on without it: it is counted as an error for now (PROVISIONAL) and
 * set right if its answer arrives after all.
 *
 * A piece of evidence stores the guess each generation made when the
 * request was asked. Its error indicator feeds every region of the guessed
 * row whose threshold is <= the stored score. Guesses are never recomputed.
 *
 * MISSING. Evidence whose answer has not arrived when the feedback timeout
 * expires stays an error for good and its id is forgotten. Counting an
 * unanswered request as an error is the worst case for the gate, so it can
 * only make the gate more cautious, and the wealth of a region with late
 * answers counted as errors is never above its true wealth, so a threshold
 * proven that way is proven.
 *
 * BLIND. A gate whose answers stop arriving cannot check itself. When
 * GATE_MISSING_RUN pieces of evidence in a row end without an answer, and
 * no answer of any request has arrived since the first of them was asked,
 * the gate drops both generations. (Evidence that ends unanswered although
 * an answer has arrived since it was asked is a lost answer, not an
 * outage: it does not count towards the run.) The gate keeps its trainer,
 * because missing answers say nothing about the model, and takes no
 * snapshot until an answer arrives again, so an outage does not use up
 * generation numbers. While blind every request is recorded, so that there
 * is an id to answer.
 *
 * PROVEN. A region is PROVEN once its log wealth has reached the
 * generation's bar; that is latched (region.certified_at). Every threshold
 * is a test of its own. Regions are nested, so the policy of a row is its
 * lowest proven threshold. Testing never stops, so the policy only moves
 * down.
 *
 *     bar = log(g (g + 1)) + log(tested rows * GATE_THRESHOLDS) - log(delta)
 *
 * PROMOTION. A candidate with at least one proven region is promoted when
 * nothing is serving, or when its policy accepts more of its own proof
 * requests than the policy the serving generation has now would accept of
 * those same requests (each generation judged by the guess it recorded). A
 * classifier candidate that has used up its budget without being promoted
 * is dropped; the budget is GATE_CANDIDATE_BUDGET_FACTOR times the proof a
 * flawless threshold needs, for each tested row. Proof uses it up, and so
 * does every request the candidate could have been tested on had it not
 * guessed a label that is never answered (the teacher's answer was a
 * tested label): a snapshot that defers everything must not be tested for
 * ever.
 *
 * DRIFT. Each serving row runs a drift detector over its spot checks of
 * accepted traffic (gate_stats.h). A spot check gets its number when it is
 * asked and counts as right until a disagreeing answer arrives, which is
 * reported the moment it arrives. The detector does not depend on the
 * order or the timing of the answers. An alarm drops the serving
 * generation and the candidate and (classifier) wipes the trainer: the
 * gate starts over.
 *
 * A spot check whose answer is given up is not a disagreement, but it may
 * hide one. The detector counts lost answers next to the disagreements and
 * raises a second alarm, at the much higher level GATE_LOST_ALARM, when
 * the two together would mean the row got worse: the gate can no longer
 * be checked. That alarm is handled like BLIND (both generations dropped,
 * the trainer kept).
 *
 * FULL. A gate that already holds GATE_MAX_PENDING unanswered requests (or
 * GATE_MAX_PENDING_BYTES of stored clues) answers ABSTAIN to everything
 * without drawing or recording anything. Requests that would only be used
 * for training are no longer recorded above GATE_TRAIN_PENDING.
 *
 * TIME. 'now_ms' is wall-clock milliseconds, between 0 and INT64_MAX -
 * GATE_MAX_FEEDBACK_TIMEOUT_MS. The gate never moves backwards: each call
 * uses max(now_ms, the largest value seen so far). The two exceptions are
 * gateReset() and gateRecover(), which drop everything that carries a time
 * and set the clock to their argument. Whoever replicates an
 * operation must replicate that effective clock, not the value it read:
 * the primary's clock also advances on requests replicas never see.
 *
 * CALL ORDER for a request: gateMaintain(now), gateEvaluate(), then
 * gatePredict(now) with the same clock. The guesses of a gateEval belong to
 * the generations as they are after that gateMaintain(); gatePredict()
 * cannot tell a stale one.
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
 * (all 0 = "not set": gateConfigValidate replaces it by alpha) and
 * class_defer (all 0). GATE_OK, or GATE_ERR on allocation failure, n = 0,
 * a config that already has classes, or a label with a zero byte inside. */
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
                              * region.certified_at != 0 once it is PROVEN */
    int32_t *policy;         /* nrows: lowest proven threshold index, -1 none */
    gateDrift *drift;        /* nrows: drift detectors (used while serving) */
    uint64_t samples;        /* evidence applied to this generation (guesses
                              * of tested rows only) */
    uint64_t errors;         /* ... of which were wrong guesses */
    uint64_t *paired;        /* candidate only, nrows * GATE_THRESHOLDS: its
                              * proof requests that the serving generation's
                              * own guess put at or above each threshold of
                              * each row (nested like the regions) */
    uint64_t audits;         /* audits applied while serving */
    uint64_t audit_errors;   /* ... of which were wrong guesses */
    uint64_t missed;         /* candidate: requests it could have been tested
                              * on had it not guessed an untested label */
    uint64_t budget;         /* candidate: dropped once samples + missed reach
                              * this without promotion; 0 = no limit */
} gateGeneration;

/* ------------------------------ Observations ----------------------------- */

#define GATE_OBS_AUDIT (1u << 0)        /* u < audit */
#define GATE_OBS_WOULD_ACCEPT (1u << 1) /* serving would have accepted */
#define GATE_OBS_CAND (1u << 2)         /* proof for generation cand_gen */
#define GATE_OBS_SERV (1u << 3)         /* proof for generation serv_gen */
#define GATE_OBS_RESOLVED (1u << 4)     /* answered, waiting for its turn */
#define GATE_OBS_PROVISIONAL (1u << 5)  /* unanswered, counted as an error
                                         * until its answer arrives */
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
    uint64_t spot;       /* its number as a spot check of the serving
                          * generation's row (drift detector); 0 if none */
    gateSparse x;        /* clues kept for training; freed once trained */
    struct gateObs *prev, *next; /* every waiting request, ascending seq */
    struct gateObs *enext;       /* evidence not applied yet, ascending seq */
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
    uint64_t missing;      /* evidence applied without its answer */
    uint64_t dropped;      /* requests forgotten unanswered that were not
                            * (or no longer) proof for anything */
    uint64_t promotions;
    uint64_t revocations;  /* times a serving generation was withdrawn */
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

    uint32_t missing_run;    /* evidence ended unanswered since the last answer */
    uint64_t answer_mark;    /* next_seq when the last answer was used: a
                              * request below it has seen an answer arrive */
    int blind;               /* no snapshot until an answer arrives (see BLIND) */

    uint64_t next_seq;       /* next request number, starts at 1 */
    uint32_t npending;       /* waiting requests, answered or not */
    uint32_t nanswered;      /* ... of which answered, waiting for their turn */
    uint32_t nevidence;      /* evidence not applied yet (the queue) */
    gateObs *oldest, *newest;
    gateObs *ehead, *etail;
    gateObs **table;         /* GATE_TABLE_SIZE chains */
    uint64_t pending_bytes;

    gateCounters stats;

    uint64_t runtime_tag;    /* owned by the embedding layer: never encoded,
                              * never touched here, 0 after decoding */
    int64_t runtime_clock;   /* likewise: 0 after gateAllocate() and after
                              * decoding, never touched here */
} gate;

/* ------------------------------- Lifecycle ------------------------------- */

/* Create a gate from a VALIDATED config (deep-copied). 'lineage' and
 * 'epoch' are caller-supplied random numbers. A score gate starts its first
 * generation at once (if only that allocation fails, the gate is returned
 * without it and the next call takes the snapshot). NULL on allocation
 * failure. */
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
 * waiting request has timed out, proof is due to move on without a late
 * answer, or a snapshot is due. Pure. */
int gateMaintenanceDue(const gate *g, int64_t now_ms);

/* Advance the clock, time out waiting requests (oldest first: unanswered
 * evidence becomes an error for good, training-only requests are dropped),
 * let proof move on without answers that are late, apply what that
 * unblocks, take a snapshot if one is due. Every mutating function calls
 * this first. */
void gateMaintain(gate *g, int64_t now_ms);

/* 1 if the gate cannot record another request (see FULL above). While
 * full, gatePredict() abstains without looking at the request. */
int gateFull(const gate *g);
/* Requests still waiting for their answer. */
uint32_t gateUnanswered(const gate *g);

/* ------------------------------- Prediction ------------------------------ */

typedef struct gatePrediction {
    int32_t label; /* label index; -1 for a score gate */
    double score;
} gatePrediction;

typedef struct gateEval {
    int has_serving, has_candidate;
    gatePrediction serving, candidate; /* a guess that is not there (no such
                                        * generation, or left out by
                                        * gateEvaluateFor) is label -1, score 0 */
} gateEval;

/* Pure: what the serving and candidate generations guess for a request.
 * Classifier: 'x' are the request's clues, 'ext_score' is ignored.
 * Score: 'x' is NULL and both "guess" (-1, ext_score).
 * GATE_ERR if a model produced a non-finite value. */
int gateEvaluate(const gate *g, const gateSparse *x, double ext_score, gateEval *ev);

/* As gateEvaluate(), but without the candidate's guess when the request
 * with draw 'u' is going to be answered by the serving generation: an
 * ACCEPT records nothing, so nobody would look at it, and the hot path
 * pays for one model instead of two. */
int gateEvaluateFor(const gate *g, const gateSparse *x, double ext_score, double u,
                    gateEval *ev);

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
 *   - otherwise the request is recorded if it is evidence, or (classifier,
 *     or any gate while blind) while fewer than GATE_TRAIN_PENDING requests
 *     are unanswered.
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
 * applied when its turn comes, or set right at once if proof had already
 * moved on without it. */
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
 * a fresh snapshot at once. Clears BLIND. */
void gateRecover(gate *g, int64_t now_ms, uint64_t new_epoch);

/* ------------------------------ Introspection ---------------------------- */

/* Error rate of a row: class_alpha[row] (classifier) or alpha (score). */
double gateRowAlpha(const gate *g, uint32_t row);
/* 1 if the row is tested (not ALWAYSDEFER). */
int gateRowTested(const gate *g, uint32_t row);
/* The bar of generation number 'gen' of this gate (see PROVEN). */
double gateBar(const gate *g, uint64_t gen);
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
 * list, evidence queue if it is evidence that was not applied yet, counts
 * and bytes. */
void gateObsLink(gate *g, gateObs *o);
/* Number of tested rows. */
uint32_t gateTestedRows(const gate *g);
/* Recompute policy[row] of a generation from its proven regions. */
void gateGenerationRefreshPolicy(const gate *g, gateGeneration *gen, uint32_t row);

#endif /* GATE_CORE_H */
