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
 * A gate decides whether a cheap prediction may replace an expensive
 * "teacher" decision. This file implements everything about that decision
 * except talking to Redis: configuration, frozen generations and their
 * evidence, pending observations, the feedback protocol, promotion,
 * revocation, resets and recovery.
 *
 * DETERMINISM CONTRACT. Every mutating function below is a deterministic
 * function of (gate state, arguments). All sources of nondeterminism are
 * arguments supplied by the caller: the wall clock ('now_ms'), the audit
 * coin ('audit'), random nonces ('lineage', 'epoch'), and -- for
 * predictions -- the frozen model outputs ('gateEval'). The Redis module
 * realizes those once on the primary, applies the mutation, and replicates
 * the realized values, so a replica or an AOF reload reaches the same state
 * by calling the same functions with the same arguments. Nothing here reads
 * a clock or a random source.
 *
 * ============================== THE PROTOCOL ==============================
 *
 * GENERATIONS. Freezing copies the mutable trainer (CLASSIFIER) or simply
 * opens a calibration epoch for the external scorer (SCORE) into an
 * immutable *generation* g = 1, 2, 3, ... numbered per lineage and never
 * reused. A generation starts as the CANDIDATE. It owns one e-process per
 * (row, threshold) region, where a row is a predicted class when the gate
 * is class-conditional and a single pooled row otherwise. Its error budget
 * is delta_g = DELTA / (g (g+1)), split equally over its regions; the
 * budget is spent at freeze time and never reclaimed.
 *
 * SAMPLING. Every request draws one independent audit bit BEFORE any
 * decision is taken, with probability gateAuditProbability(): AUDIT while a
 * policy is serving, COLDAUDIT otherwise. A request whose bit is set while
 * at least one generation exists is ELIGIBLE: its teacher label is
 * statistical evidence and MUST be delivered. Requests whose bit is clear
 * are never evidence; in CLASSIFIER mode they still get an observation so
 * the label can train the trainer, in SCORE mode they get none.
 *
 * EVIDENCE. An eligible observation stores the predictions the candidate
 * and the serving generation made at prediction time. When its label
 * arrives, the error indicator of each stored prediction is fed to the
 * regions of the generation that made it: every region of the predicted
 * row whose threshold is <= the stored score. Predictions are never
 * recomputed with newer weights.
 *
 * ORDER. Evidence is applied strictly in observation order. Eligible
 * observations form a FIFO; a labelled observation waits until every older
 * eligible observation has been labelled or has expired. The time at which
 * the teacher answers therefore cannot select the calibration stream.
 *
 * CERTIFICATION AND PROMOTION. A region is certified the first time its log
 * mixture wealth reaches the generation's threshold. When the first region
 * of the candidate certifies, the candidate is promoted: it becomes the
 * SERVING generation (replacing any previous one), and its policy for each
 * row is the LOWEST certified threshold of that row. Rows with no certified
 * threshold, and ALWAYSDEFER classes, always defer.
 *
 * AFTER PROMOTION. With EXPAND off (the specification's rule) testing stops:
 * the policy is fixed for the life of the generation. With EXPAND on the
 * generation keeps consuming its eligible samples, and whenever a lower
 * threshold (or another class row) certifies the policy is extended to it.
 * This spends no additional budget: every region of the generation was
 * allotted its own delta_region at freeze time and the e-processes are
 * valid at every stopping time, so all regions ever certified are jointly
 * valid with probability >= 1 - delta_g.
 *
 * SERVING DECISION. A request is a WOULD-ACCEPT when a serving generation
 * exists, the predicted row has a policy threshold, and score >= that
 * threshold. A would-accept with a clear audit bit is ACCEPTed (no teacher
 * call, no observation). A would-accept with the audit bit set is an AUDIT:
 * the teacher runs and the outcome feeds the serving drift window.
 * Everything else is an ABSTAIN.
 *
 * DRIFT GUARD. Each serving row keeps the last GATE_DRIFT_WINDOW audit
 * outcomes. The whole serving generation is revoked (DRIFT_DETECTED) when a
 * row has at least GATE_DRIFT_MIN audits in its window and their error
 * fraction exceeds alpha_row + DRIFTMARGIN. This is a heuristic alarm, not
 * a statistical guarantee.
 *
 * MISSING EVIDENCE. An eligible observation whose label does not arrive
 * within OBSERVATIONTTL, or that could not be stored (RESOURCE_LIMIT), is
 * handled by the ONMISSING policy:
 *
 *   REVOKE  (the specification's rule) the evidence it belonged to is
 *           invalid: a candidate it was sampled for is discarded; a serving
 *           generation is revoked (FEEDBACK_INTEGRITY) if the observation
 *           was a would-accept, otherwise its post-promotion testing is
 *           closed (the policy stops expanding but keeps serving, since
 *           certifications already made are unaffected by later samples).
 *   ERROR   the missing label is imputed as a disagreement (E = 1) for
 *           every prediction stored in the observation. Imputing the worst
 *           case can only lower wealth and raise the drift count, so
 *           validity is preserved without discarding anything.
 *
 * RECOVERY. A gate whose state may have been rolled back (loaded from disk,
 * received by replication and then promoted, restored from a dump) must be
 * fenced with gateFence(): certification and pending observations are
 * dropped, observation ids change, and no new generation may be frozen
 * until an operator acknowledges with gateRecover(). The budget ledger
 * continues from the loaded value; it is never replenished.
 *
 * TIME. 'now_ms' is wall-clock milliseconds. The gate never moves backwards:
 * each call uses max(now_ms, the largest value seen so far).
 */

#ifndef GATE_CORE_H
#define GATE_CORE_H

#include <stddef.h>
#include <stdint.h>
#include "gate_defs.h"
#include "gate_stats.h"
#include "gate_model.h"
#include "gate_features.h"

/* ------------------------------ Enumerations ----------------------------- */

#define GATE_MODE_SCORE 0
#define GATE_MODE_CLASSIFIER 1

/* Lifecycle state, derived by gateState(). */
typedef enum gateStateId {
    GATE_STATE_COLD = 0,    /* no generation, never revoked since the last reset */
    GATE_STATE_CALIBRATING, /* a candidate is under test, nothing is serving */
    GATE_STATE_ACTIVE,      /* a certified generation is serving */
    GATE_STATE_REVOKED,     /* serving was invalidated, or recovery is required */
    GATE_STATE_PAUSED       /* administratively paused (shadow mode) */
} gateStateId;

/* Why a request was not accepted / why no policy is serving. The numeric
 * values are part of the persistence format: append only, never renumber. */
typedef enum gateReason {
    GATE_REASON_NONE = 0,
    GATE_REASON_COLD_START = 1,
    GATE_REASON_BELOW_THRESHOLD = 2,
    GATE_REASON_CLASS_DEFERRED = 3,
    GATE_REASON_DRIFT_DETECTED = 4,
    GATE_REASON_CERT_EXPIRED = 5,
    GATE_REASON_PAUSED = 6,
    GATE_REASON_TEACHER_CHANGED = 7,
    GATE_REASON_RESOURCE_LIMIT = 8,
    GATE_REASON_FEEDBACK_INTEGRITY = 9,
    GATE_REASON_RECOVERY_REQUIRED = 10,
    GATE_REASON_SCHEMA_CHANGED = 11,
    GATE_REASON_CONFIG_CHANGED = 12,
    GATE_REASON_COUNT = 13
} gateReason;

#define GATE_ACTION_ACCEPT 0
#define GATE_ACTION_ABSTAIN 1
#define GATE_ACTION_AUDIT 2

#define GATE_ONMISSING_REVOKE 0
#define GATE_ONMISSING_ERROR 1

typedef enum gateResetScope {
    GATE_RESET_CANDIDATE = 0, /* cancel the candidate only */
    GATE_RESET_CALIBRATION,   /* discard candidate and serving, keep the trainer */
    GATE_RESET_MODEL,         /* ... and zero the trainer weights */
    GATE_RESET_ALL            /* ... and drop pending observations and counters */
} gateResetScope;

typedef enum gateFeedbackStatus {
    GATE_FB_APPLIED = 0,      /* consumed: trained and/or queued as evidence */
    GATE_FB_DUPLICATE,        /* this observation was already consumed */
    GATE_FB_EXPIRED,          /* its OBSERVATIONTTL elapsed before feedback */
    GATE_FB_INVALIDATED,      /* dropped by a reset or a teacher/schema change */
    GATE_FB_UNKNOWN,          /* not an id of this gate's current epoch */
    GATE_FB_TEACHER_MISMATCH, /* labelled by a different teacher version */
    GATE_FB_STATUS_COUNT
} gateFeedbackStatus;

/* Canonical upper-case names used in replies ("COLD_START", "APPLIED", ...).
 * Return "UNKNOWN" for out-of-range values; never NULL. */
const char *gateStateName(gateStateId s);
const char *gateReasonName(gateReason r);
const char *gateActionName(int action);
const char *gateFeedbackStatusName(gateFeedbackStatus s);

/* ----------------------------- Configuration ----------------------------- */

typedef struct gateConfig {
    int mode;                   /* GATE_MODE_* */
    double alpha;               /* risk target, (0,1) */
    double delta;               /* lifetime false-certification budget, (0,1) */
    double audit;               /* sampling probability while serving, (0,1] */
    double cold_audit;          /* sampling probability otherwise, (0,1] */
    uint32_t nthresholds;       /* T */
    uint32_t nclasses;          /* K; 0 only in SCORE mode without CLASSES */
    char **labels;              /* K NUL-terminated class labels, config order */
    gateSchema schema;          /* CLASSIFIER input schema (zeroed in SCORE) */
    double lr;                  /* learning rate eta */
    double l2;                  /* L2 coefficient lambda */
    uint64_t max_memory;        /* per-gate byte limit */
    uint32_t max_pending;       /* max pending observations */
    int64_t observation_ttl_ms;
    int64_t cert_ttl_ms;
    char *teacher;              /* teacher version (NUL-terminated) */
    char *schema_version;       /* feature / scoring policy version */
    int class_mode;             /* 1 = one region row per predicted class */
    double *class_alpha;        /* K per-class risk targets (class_mode) */
    uint8_t *class_defer;       /* K ALWAYSDEFER flags (class_mode) */
    double drift_margin;        /* < 0 means "alpha of the row" (the default) */
    int on_missing;             /* GATE_ONMISSING_* */
    int expand;                 /* 1 = keep testing and extending after promotion */
} gateConfig;

/* Set every field to its documented default (mode SCORE, alpha 0, no
 * classes, ...). The parser then overrides fields from the command line. */
void gateConfigInit(gateConfig *cfg);

/* Release every heap field of 'cfg' (not 'cfg' itself) and zero it. */
void gateConfigFree(gateConfig *cfg);

/* Deep copy. GATE_OK / GATE_ERR. */
int gateConfigCopy(gateConfig *dst, const gateConfig *src);

/* Check every invariant: ranges, hard limits, mode-specific rules (SCORE
 * accepts no classifier parameters; CLASSIFIER needs >= 2 distinct labels
 * and a non-empty input schema; class options need CLASSES; every class
 * alpha <= alpha; not every class deferred; 0 <= lr*l2 < 1; MAXMEMORY large
 * enough for the fixed footprint, see gateFixedBytes()). On failure writes
 * a human readable message (no trailing newline) into 'err' and returns
 * GATE_ERR. */
int gateConfigValidate(const gateConfig *cfg, char *err, size_t errlen);

/* Worst-case bytes of the non-pending part of a gate with this config: the
 * gate itself, the trainer, and two generations (candidate + serving). */
uint64_t gateFixedBytes(const gateConfig *cfg);

/* Index of 'label' in cfg->labels, or -1. */
int gateConfigLabelIndex(const gateConfig *cfg, const char *label, size_t len);

/* ------------------------------ Generations ------------------------------ */

typedef struct gateGeneration {
    uint64_t gen;            /* generation index g >= 1 */
    int64_t frozen_ms;
    int64_t promoted_ms;     /* 0 while it is the candidate */
    int64_t expires_ms;      /* promoted_ms + CERTTTL; 0 while candidate */
    double delta_gen;        /* DELTA / (g (g+1)) */
    double delta_region;     /* delta_gen / regions_tested */
    double log_threshold;    /* -log(delta_region) */
    uint32_t regions_tested; /* tested rows * T */
    gateModel model;         /* frozen weights (CLASSIFIER); zeroed in SCORE */
    gateRegion *regions;     /* nrows * T, index row * T + t */
    int32_t *policy;         /* nrows: lowest certified threshold index, -1 = none */
    gateDriftWindow *drift;  /* nrows: audits of accepted traffic (serving only) */
    uint64_t samples;        /* eligible samples applied to this generation */
    uint64_t errors;         /* ... of which were disagreements */
    uint64_t imputed;        /* ... of which were imputed (ONMISSING ERROR) */
    uint32_t policy_updates; /* times the policy was set or extended */
    int closed;              /* 1 = no further region testing */
} gateGeneration;

/* ------------------------------ Observations ----------------------------- */

#define GATE_OBS_AUDIT (1u << 0)        /* the audit bit was set */
#define GATE_OBS_WOULD_ACCEPT (1u << 1) /* serving would have accepted */
#define GATE_OBS_CAND (1u << 2)         /* eligible for generation cand_gen */
#define GATE_OBS_SERV (1u << 3)         /* eligible for generation serv_gen */
#define GATE_OBS_RESOLVED (1u << 4)     /* feedback recorded, awaiting its turn */
#define GATE_OBS_IMPUTED (1u << 5)      /* resolved by imputation, not a label */
#define GATE_OBS_ELIGIBLE (GATE_OBS_CAND | GATE_OBS_SERV)

typedef struct gateObs {
    uint64_t seq;        /* observation sequence number, >= 1 */
    int64_t created_ms;
    uint32_t flags;      /* GATE_OBS_* */
    int32_t cand_label;  /* frozen candidate prediction (-1: none / no label) */
    int32_t serv_label;  /* frozen serving prediction */
    double cand_score;
    double serv_score;
    uint64_t cand_gen;   /* 0 = no candidate at prediction time */
    uint64_t serv_gen;   /* 0 = nothing serving at prediction time */
    int32_t result;      /* resolved: teacher class index (CLASSIFIER) or the
                          * CORRECT bit (SCORE); -1 while unresolved */
    gateSparse x;        /* features kept for training; released once trained */
    struct gateObs *prev, *next; /* all pending observations, ascending seq */
    struct gateObs *enext;       /* eligible FIFO, ascending seq */
    struct gateObs *hnext;       /* hash chain */
} gateObs;

/* ----------------------------- Counters ---------------------------------- */

typedef struct gateCounters {
    uint64_t requests;        /* GPREDICT calls */
    uint64_t accepted;        /* ACCEPT replies: teacher calls avoided */
    uint64_t audited;         /* AUDIT replies */
    uint64_t abstained;       /* ABSTAIN replies */
    uint64_t would_accept;    /* requests inside the serving policy region */
    uint64_t abstain_reason[GATE_REASON_COUNT];
    uint64_t observations;    /* observation ids issued */
    uint64_t eligible;        /* ... of which were eligible evidence */
    uint64_t feedback[GATE_FB_STATUS_COUNT]; /* GLEARN outcomes */
    uint64_t expired;         /* observations that expired unlabelled */
    uint64_t expired_eligible;/* ... of which were eligible */
    uint64_t resource_limited;/* requests that could not be recorded */
    uint64_t imputed;         /* eligible samples imputed as errors */
    uint64_t train_rejected;  /* SGD updates rejected as non-finite */
    uint64_t promotions;
    uint64_t revocations;
    uint64_t candidates_discarded;
} gateCounters;

/* --------------------------------- Gate ---------------------------------- */

typedef struct gate {
    gateConfig cfg;
    double *grid;            /* cfg.nthresholds strictly increasing thresholds */
    uint32_t nrows;          /* region rows: K if class_mode, else 1 */
    gateBets *bets;          /* nrows: e-process increments for the row alpha */

    uint64_t lineage;        /* random id of this key's budget ledger */
    uint64_t epoch;          /* random nonce embedded in observation ids */
    int64_t created_ms;
    int64_t now_ms;          /* largest clock value applied so far */

    uint64_t generations;    /* generations frozen in this lineage (ledger) */
    uint32_t recoveries;     /* acknowledged recoveries (gateRecover) */

    gateModel trainer;       /* mutable model (CLASSIFIER); zeroed in SCORE */
    gateGeneration *candidate;
    gateGeneration *serving;

    int paused;
    int recovery_required;
    gateReason revoked_reason; /* why nothing is serving right now; NONE when
                                * serving, or never revoked since creation /
                                * the last reset / recovery */
    gateReason last_revoked_reason; /* history of the most recent revocation */
    int64_t last_revoked_ms;        /* (kept across re-promotion; cleared only */
    uint64_t last_revoked_gen;      /*  by GRESET ALL). 0 / NONE = never       */

    uint64_t next_seq;        /* next observation sequence number, starts at 1 */
    uint64_t expired_through; /* every seq <= this has outlived its TTL */
    uint64_t invalid_below;   /* every seq < this was dropped by an invalidation */
    uint32_t npending;
    uint32_t neligible;       /* observations currently in the eligible FIFO */
    gateObs *oldest, *newest; /* all pending, ascending seq */
    gateObs *ehead, *etail;   /* eligible FIFO */
    gateObs **table;          /* seq -> observation, chained */
    uint32_t table_size;      /* power of two, 0 when table == NULL */
    uint64_t pending_bytes;   /* bytes attributed to pending observations */
    uint64_t fixed_bytes;     /* gateFixedBytes(&cfg), cached */

    gateCounters stats;

    uint64_t runtime_tag;     /* owned by the embedding layer: never serialized,
                               * never touched by this file, 0 after decode */
} gate;

/* ------------------------------ Lifecycle -------------------------------- */

/* Create a gate from a VALIDATED config, which is deep-copied. 'lineage' and
 * 'epoch' are caller-supplied random 64-bit values. Returns NULL on
 * allocation failure. The new gate is COLD. */
gate *gateCreate(const gateConfig *cfg, uint64_t lineage, uint64_t epoch, int64_t now_ms);

/* Release everything. O(pending observations). */
void gateFree(gate *g);

/* Bytes currently attributed to the gate (struct, config, trainer, live
 * generations, pending observations and their index). A deterministic
 * function of the state: it uses requested sizes, not allocator slack. */
uint64_t gateMemoryUsage(const gate *g);

/* Rough count of heap objects, for lazy-free effort estimation. */
uint64_t gateFreeEffort(const gate *g);

/* Derived lifecycle state:
 *   paused                         -> PAUSED
 *   else serving != NULL           -> ACTIVE
 *   else recovery_required         -> REVOKED
 *   else candidate != NULL         -> CALIBRATING
 *   else revoked_reason != NONE    -> REVOKED
 *   else                           -> COLD                                  */
gateStateId gateState(const gate *g);

/* The reason a request that is not a would-accept is given right now
 * (ignoring per-request BELOW_THRESHOLD / CLASS_DEFERRED / RESOURCE_LIMIT):
 * RECOVERY_REQUIRED, else PAUSED, else NONE when serving, else
 * revoked_reason if set, else COLD_START. */
gateReason gateIdleReason(const gate *g);

/* Read-only preview of what the lazy maintenance would do at 'now_ms',
 * without mutating: the state and idle reason the gate will have once a
 * mutating call observes that the certificate expired or (ONMISSING
 * REVOKE) that an unlabelled would-accept audit outlived its TTL. Used by
 * GINFO. 'fenced' != 0 previews gateFence() as well. */
void gatePreview(const gate *g, int64_t now_ms, int fenced,
                 gateStateId *state, gateReason *reason);

/* Probability with which the caller must draw the audit bit for the next
 * request: cfg.audit when a generation is serving, cfg.cold_audit
 * otherwise. Call after gateMaintain() so that expiry is accounted. */
double gateAuditProbability(const gate *g);

/* Advance the clock and apply every time-derived effect, in this order:
 *   1. certificate expiry -> revoke the serving generation (CERT_EXPIRED);
 *   2. observation expiry, oldest first: unlabelled eligible observations
 *      go through the ONMISSING policy, unlabelled training-only ones are
 *      dropped; evidence unblocked by an expiry is applied.
 * Every mutating function below calls this first, so callers only need it
 * before gateAuditProbability() / gateEvaluate(). Work is bounded by the
 * number of pending observations. */
void gateMaintain(gate *g, int64_t now_ms);

/* ------------------------------- Prediction ------------------------------ */

typedef struct gatePrediction {
    int32_t label; /* class index, or -1 when the gate has no classes */
    double score;
} gatePrediction;

/* Frozen predictions for one request. has_* is 0 when that generation does
 * not exist (the prediction is then ignored). */
typedef struct gateEval {
    int has_serving, has_candidate;
    gatePrediction serving, candidate;
} gateEval;

/* Pure: compute the predictions of the serving and candidate generations.
 * CLASSIFIER: 'x' is the request's feature vector; ext_* are ignored.
 * SCORE: 'x' is NULL; both generations "predict" (ext_label, ext_score).
 * Returns GATE_ERR if a model produced a non-finite value. */
int gateEvaluate(const gate *g, const gateSparse *x, double ext_score,
                 int32_t ext_label, gateEval *ev);

typedef struct gatePredictResult {
    int action;          /* GATE_ACTION_* */
    gateReason reason;   /* ABSTAIN only, else NONE */
    int32_t label;       /* serving prediction's class, -1 if none */
    double score;        /* serving prediction's score (ACCEPT / AUDIT) */
    uint64_t generation; /* serving generation, 0 if nothing is serving */
    double risk_target;  /* alpha of the accepting row (ACCEPT) */
    uint64_t seq;        /* observation registered for feedback, 0 = none */
} gatePredictResult;

/* Register one request. 'audit' is the realized audit bit (0/1) drawn with
 * gateAuditProbability(); 'ev' the realized predictions.
 *
 * Decision, in order:
 *   - recovery_required: ABSTAIN RECOVERY_REQUIRED, no observation.
 *   - would-accept (see file comment), not paused, audit clear: ACCEPT.
 *   - would-accept, not paused, audit set: AUDIT.
 *   - otherwise ABSTAIN with reason PAUSED if paused, else CLASS_DEFERRED /
 *     BELOW_THRESHOLD when a generation is serving, else gateIdleReason().
 *
 * Observation: every non-ACCEPT outcome registers an observation when it is
 * useful, i.e. when it is eligible (audit set and a candidate and/or a
 * serving generation exists) or the gate is a CLASSIFIER (the label trains
 * the trainer). Eligibility flags: GATE_OBS_CAND if a candidate exists;
 * GATE_OBS_SERV if a generation is serving and either the request is a
 * would-accept (drift window) or the generation is still testing
 * (cfg.expand && !closed).
 *
 * Limits: if npending == max_pending or the observation would push memory
 * past max_memory, nothing is stored and the result is ABSTAIN
 * RESOURCE_LIMIT with seq 0; if the request was eligible the ONMISSING
 * policy is applied to it immediately.
 *
 * 'x' (CLASSIFIER, may be NULL otherwise): when an observation is stored
 * the gate takes ownership of x's arrays and resets *x to empty; otherwise
 * x is left untouched and the caller frees it. Passing x == NULL when the
 * gate needs to store features returns GATE_ERR without mutating anything
 * (this can only happen through an inconsistent replication stream).
 *
 * Returns GATE_OK and fills 'res'. */
int gatePredict(gate *g, int64_t now_ms, int audit, const gateEval *ev,
                gateSparse *x, gatePredictResult *res);

/* 1 if gatePredict() with these arguments would register an observation
 * that needs the feature vector. Lets the caller skip feature extraction
 * and replication of the raw input for ACCEPTed requests. Call after
 * gateMaintain(). */
int gatePredictNeedsFeatures(const gate *g, int audit, const gateEval *ev);

/* ------------------------------- Feedback -------------------------------- */

#define GATE_FEEDBACK_LABEL 0   /* CLASSIFIER: hard teacher label */
#define GATE_FEEDBACK_PROBS 1   /* CLASSIFIER: teacher distribution */
#define GATE_FEEDBACK_CORRECT 2 /* SCORE: candidate agreed with the teacher */

typedef struct gateFeedback {
    int kind;            /* GATE_FEEDBACK_* */
    int32_t label;       /* LABEL: class index. PROBS: argmax, lowest index on
                          * ties (filled by the caller). CORRECT: unused */
    const double *probs; /* PROBS: K values, validated by the caller */
    int correct;         /* CORRECT: 0 or 1 */
} gateFeedback;

/* Deliver teacher feedback for observation ('epoch', 'seq').
 *
 *   UNKNOWN           epoch != g->epoch, seq == 0 or seq >= next_seq.
 *   INVALIDATED       not pending and seq < invalid_below.
 *   EXPIRED           not pending and seq <= expired_through.
 *   DUPLICATE         not pending otherwise, or pending but already resolved.
 *   TEACHER_MISMATCH  'teacher' != NULL and differs from cfg.teacher; the
 *                     observation stays pending.
 *   APPLIED           the observation is consumed: its outcome is recorded;
 *                     in CLASSIFIER mode the trainer takes one SGD step on
 *                     the stored features (after the outcome is recorded);
 *                     if it is eligible it is applied as evidence when its
 *                     turn comes, otherwise it is released at once.
 *
 * Only APPLIED changes anything besides the feedback counters. The caller
 * guarantees 'fb' matches the gate's mode. */
gateFeedbackStatus gateLearn(gate *g, int64_t now_ms, uint64_t epoch, uint64_t seq,
                             const gateFeedback *fb, const char *teacher);

/* Direct supervised training (GADD): one SGD step of the trainer towards
 * q[0..K-1]. Never statistical evidence. CLASSIFIER only. Returns GATE_OK,
 * or GATE_ERR if the update was rejected as non-finite (nothing changes
 * except stats.train_rejected). */
int gateTrain(gate *g, int64_t now_ms, const gateSparse *x, const double *q);

/* ------------------------------ Generations ------------------------------ */

#define GATE_FREEZE_OK 0
#define GATE_FREEZE_ERR_RECOVERY 1  /* recovery required */
#define GATE_FREEZE_ERR_CANDIDATE 2 /* a candidate is already under test */
#define GATE_FREEZE_ERR_UNTRAINED 3 /* CLASSIFIER trainer has no updates */
#define GATE_FREEZE_ERR_NOMEM 4

/* Freeze a new candidate generation and spend its budget. On success
 * '*gen' receives the new generation index. */
int gateFreeze(gate *g, int64_t now_ms, uint64_t *gen);

/* Reset, see gateResetScope. Never touches the ledger (generations), the
 * lineage, the pause flag or recovery_required. CALIBRATION and above
 * clear revoked_reason (the gate becomes COLD). ALL also sets
 * invalid_below = next_seq and zeroes the counters. */
void gateReset(gate *g, int64_t now_ms, gateResetScope scope);

void gatePause(gate *g, int64_t now_ms);
void gateResume(gate *g, int64_t now_ms);

/* Change the serving audit probability. A different value invalidates
 * certification: the candidate is discarded and a serving generation is
 * revoked (CONFIG_CHANGED). The same value is a no-op. */
void gateSetAudit(gate *g, int64_t now_ms, double audit);

/* Change the teacher / schema version. A different value discards the
 * candidate, revokes a serving generation (TEACHER_CHANGED /
 * SCHEMA_CHANGED), zeroes the trainer and drops every pending observation
 * (invalid_below = next_seq). The same value is a no-op. GATE_OK/GATE_ERR
 * (allocation). */
int gateSetTeacher(gate *g, int64_t now_ms, const char *teacher);
int gateSetSchemaVersion(gate *g, int64_t now_ms, const char *version);

/* ------------------------------- Recovery -------------------------------- */

/* Fail closed after a possible rollback: discard candidate and serving, drop
 * every pending observation, switch to 'new_epoch' (so every outstanding
 * observation id becomes UNKNOWN), set recovery_required and
 * revoked_reason = RECOVERY_REQUIRED. The trainer and the ledger are kept.
 * Idempotent apart from the epoch change. */
void gateFence(gate *g, int64_t now_ms, uint64_t new_epoch);

/* Operator acknowledgement: start a new declared risk lifecycle. Clears
 * recovery_required and revoked_reason (the gate becomes COLD), increments
 * 'recoveries' and switches to 'new_epoch'. Returns GATE_ERR (no change) if
 * the gate does not require recovery. */
int gateRecover(gate *g, int64_t now_ms, uint64_t new_epoch);

/* ------------------------------ Introspection ---------------------------- */

/* Budget ledger. */
double gateBudgetSpentNow(const gate *g);     /* DELTA * g/(g+1) */
double gateBudgetRemainingNow(const gate *g); /* DELTA / (g+1) */

/* Risk target of a row: class_alpha[row] in class mode, else alpha. */
double gateRowAlpha(const gate *g, uint32_t row);

/* Estimated coverage of a generation's current policy: the fraction of its
 * applied eligible samples that fell inside the policy region. 0 when it
 * has no samples. An empirical estimate, not a bound. */
double gateGenerationCoverage(const gate *g, const gateGeneration *gen);

/* Age in ms of the oldest pending observation at g->now_ms (0 if none). */
int64_t gateOldestPendingAge(const gate *g);

/* Look up a pending observation (NULL if not pending). Exposed for tests. */
gateObs *gateFindObservation(const gate *g, uint64_t seq);

/* Internal consistency check used by tests and debug builds: list links,
 * counters, byte accounting, FIFO order, policy vs certified regions.
 * Returns 1 if consistent, else 0 with a message in 'err'. */
int gateCheckInvariants(const gate *g, char *err, size_t errlen);

#endif /* GATE_CORE_H */
