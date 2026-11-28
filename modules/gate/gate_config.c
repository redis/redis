/*
 * Gate data type: configuration defaults, validation and name tables.
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
#include "gate_internal.h"

/* ------------------------------ Name tables ------------------------------ */

const char *gateStateName(gateStateId s) {
    switch (s) {
    case GATE_STATE_COLD: return "COLD";
    case GATE_STATE_CALIBRATING: return "CALIBRATING";
    case GATE_STATE_ACTIVE: return "ACTIVE";
    case GATE_STATE_REVOKED: return "REVOKED";
    case GATE_STATE_PAUSED: return "PAUSED";
    }
    return "UNKNOWN";
}

const char *gateReasonName(gateReason r) {
    switch (r) {
    case GATE_REASON_NONE: return "NONE";
    case GATE_REASON_COLD_START: return "COLD_START";
    case GATE_REASON_BELOW_THRESHOLD: return "BELOW_THRESHOLD";
    case GATE_REASON_CLASS_DEFERRED: return "CLASS_DEFERRED";
    case GATE_REASON_DRIFT_DETECTED: return "DRIFT_DETECTED";
    case GATE_REASON_CERT_EXPIRED: return "CERT_EXPIRED";
    case GATE_REASON_PAUSED: return "PAUSED";
    case GATE_REASON_TEACHER_CHANGED: return "TEACHER_CHANGED";
    case GATE_REASON_RESOURCE_LIMIT: return "RESOURCE_LIMIT";
    case GATE_REASON_FEEDBACK_INTEGRITY: return "FEEDBACK_INTEGRITY";
    case GATE_REASON_RECOVERY_REQUIRED: return "RECOVERY_REQUIRED";
    case GATE_REASON_SCHEMA_CHANGED: return "SCHEMA_CHANGED";
    case GATE_REASON_CONFIG_CHANGED: return "CONFIG_CHANGED";
    case GATE_REASON_COUNT: break;
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

const char *gateFeedbackStatusName(gateFeedbackStatus s) {
    switch (s) {
    case GATE_FB_APPLIED: return "APPLIED";
    case GATE_FB_DUPLICATE: return "DUPLICATE";
    case GATE_FB_EXPIRED: return "EXPIRED";
    case GATE_FB_INVALIDATED: return "INVALIDATED";
    case GATE_FB_UNKNOWN: return "UNKNOWN";
    case GATE_FB_TEACHER_MISMATCH: return "TEACHER_MISMATCH";
    case GATE_FB_STATUS_COUNT: break;
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
    cfg->mode = GATE_MODE_SCORE;
    cfg->alpha = 0;                       /* mandatory: no default */
    cfg->delta = GATE_DEFAULT_DELTA;
    cfg->audit = GATE_DEFAULT_AUDIT;
    cfg->cold_audit = GATE_DEFAULT_AUDIT; /* parser: follows AUDIT unless set */
    cfg->nthresholds = GATE_DEFAULT_THRESHOLDS;
    cfg->lr = GATE_DEFAULT_LR;
    cfg->l2 = GATE_DEFAULT_L2;
    cfg->max_memory = GATE_DEFAULT_MAX_MEMORY;
    cfg->max_pending = GATE_DEFAULT_MAX_PENDING;
    cfg->observation_ttl_ms = GATE_DEFAULT_OBSERVATION_TTL_MS;
    cfg->cert_ttl_ms = GATE_DEFAULT_CERT_TTL_MS;
    cfg->drift_margin = -1;               /* "the row's alpha" */
    cfg->on_missing = GATE_ONMISSING_REVOKE;
    cfg->expand = 0;
    /* teacher / schema_version stay NULL: the parser installs
     * GATE_DEFAULT_VERSION when the option is absent. */
}

void gateConfigFree(gateConfig *cfg) {
    if (cfg == NULL) return;
    if (cfg->labels) {
        for (uint32_t i = 0; i < cfg->nclasses; i++) gate_free(cfg->labels[i]);
        gate_free(cfg->labels);
    }
    gate_free(cfg->teacher);
    gate_free(cfg->schema_version);
    gate_free(cfg->class_alpha);
    gate_free(cfg->class_defer);
    memset(cfg, 0, sizeof(*cfg));
}

int gateConfigCopy(gateConfig *dst, const gateConfig *src) {
    *dst = *src;
    dst->labels = NULL;
    dst->teacher = NULL;
    dst->schema_version = NULL;
    dst->class_alpha = NULL;
    dst->class_defer = NULL;

    uint32_t k = src->nclasses;
    if (k > 0 && src->labels) {
        dst->labels = gate_calloc(k, sizeof(char *));
        if (dst->labels == NULL) goto oom;
        for (uint32_t i = 0; i < k; i++) {
            dst->labels[i] = gateStrdupLen(src->labels[i], strlen(src->labels[i]));
            if (dst->labels[i] == NULL) goto oom;
        }
    }
    if (src->teacher) {
        dst->teacher = gateStrdupLen(src->teacher, strlen(src->teacher));
        if (dst->teacher == NULL) goto oom;
    }
    if (src->schema_version) {
        dst->schema_version = gateStrdupLen(src->schema_version, strlen(src->schema_version));
        if (dst->schema_version == NULL) goto oom;
    }
    if (src->class_alpha) {
        dst->class_alpha = gate_malloc(sizeof(double) * k);
        if (dst->class_alpha == NULL) goto oom;
        memcpy(dst->class_alpha, src->class_alpha, sizeof(double) * k);
    }
    if (src->class_defer) {
        dst->class_defer = gate_malloc(k);
        if (dst->class_defer == NULL) goto oom;
        memcpy(dst->class_defer, src->class_defer, k);
    }
    return GATE_OK;

oom:
    gateConfigFree(dst);
    return GATE_ERR;
}

int gateConfigLabelIndex(const gateConfig *cfg, const char *label, size_t len) {
    for (uint32_t i = 0; i < cfg->nclasses; i++) {
        if (strlen(cfg->labels[i]) == len && memcmp(cfg->labels[i], label, len) == 0)
            return (int)i;
    }
    return -1;
}

uint32_t gateTableSize(uint32_t max_pending) {
    uint32_t size = 16;
    while (size < max_pending) size <<= 1;
    return size;
}

uint32_t gateTestedRegions(const gateConfig *cfg) {
    uint32_t rows = gateConfigRows(cfg), tested = 0;
    for (uint32_t r = 0; r < rows; r++)
        if (gateRowTested(cfg, r)) tested++;
    return tested * cfg->nthresholds;
}

uint64_t gateFixedBytes(const gateConfig *cfg) {
    uint32_t k = cfg->nclasses;
    uint32_t rows = gateConfigRows(cfg);
    uint32_t dim = gateSchemaDim(&cfg->schema);
    uint64_t model = 0;
    if (cfg->mode == GATE_MODE_CLASSIFIER) model = gateModelBytes(dim, k);

    /* The gate itself: struct, config strings and arrays, grid, bets. */
    uint64_t base = sizeof(gate);
    base += (uint64_t)k * sizeof(char *);
    if (cfg->labels)
        for (uint32_t i = 0; i < k; i++) base += strlen(cfg->labels[i]) + 1;
    if (cfg->teacher) base += strlen(cfg->teacher) + 1;
    if (cfg->schema_version) base += strlen(cfg->schema_version) + 1;
    if (cfg->class_mode) base += (uint64_t)k * (sizeof(double) + 1);
    base += (uint64_t)cfg->nthresholds * sizeof(double);
    base += (uint64_t)rows * sizeof(gateBets);
    base += (uint64_t)gateTableSize(cfg->max_pending) * sizeof(gateObs *);

    /* One generation: struct, frozen model, regions, policy, drift windows. */
    uint64_t gen = sizeof(gateGeneration) + model;
    gen += (uint64_t)rows * cfg->nthresholds * sizeof(gateRegion);
    gen += (uint64_t)rows * sizeof(int32_t);
    gen += (uint64_t)rows * sizeof(gateDriftWindow);

    /* Trainer plus candidate plus serving. */
    return base + model + 2 * gen;
}

#define GATE_CFG_FAIL(...) do { \
    if (err && errlen) snprintf(err, errlen, __VA_ARGS__); \
    return GATE_ERR; \
} while (0)

/* Open unit interval, rejecting NaN. */
static int gateInOpenUnit(double v) { return v > 0.0 && v < 1.0; }

int gateConfigValidate(const gateConfig *cfg, char *err, size_t errlen) {
    if (cfg->mode != GATE_MODE_SCORE && cfg->mode != GATE_MODE_CLASSIFIER)
        GATE_CFG_FAIL("invalid MODE");
    if (!gateInOpenUnit(cfg->alpha))
        GATE_CFG_FAIL("ALPHA must be greater than 0 and less than 1");
    if (!gateInOpenUnit(cfg->delta))
        GATE_CFG_FAIL("DELTA must be greater than 0 and less than 1");
    if (!(cfg->audit > 0.0 && cfg->audit <= 1.0))
        GATE_CFG_FAIL("AUDIT must be greater than 0 and at most 1");
    if (!(cfg->cold_audit > 0.0 && cfg->cold_audit <= 1.0))
        GATE_CFG_FAIL("COLDAUDIT must be greater than 0 and at most 1");
    if (cfg->nthresholds < GATE_MIN_THRESHOLDS || cfg->nthresholds > GATE_MAX_THRESHOLDS)
        GATE_CFG_FAIL("THRESHOLDS must be between %d and %d",
                      GATE_MIN_THRESHOLDS, GATE_MAX_THRESHOLDS);

    /* Classes. */
    uint32_t k = cfg->nclasses;
    if (cfg->mode == GATE_MODE_CLASSIFIER && k < GATE_MIN_CLASSES)
        GATE_CFG_FAIL("CLASSIFIER mode requires CLASSES with at least %d labels",
                      GATE_MIN_CLASSES);
    if (k != 0 && (k < GATE_MIN_CLASSES || k > GATE_MAX_CLASSES))
        GATE_CFG_FAIL("CLASSES must list between %d and %d labels",
                      GATE_MIN_CLASSES, GATE_MAX_CLASSES);
    if (k != 0 && cfg->labels == NULL) GATE_CFG_FAIL("missing class labels");
    for (uint32_t i = 0; i < k; i++) {
        const char *l = cfg->labels[i];
        if (l == NULL || !gateValidName(l, strlen(l), GATE_MAX_LABEL_BYTES))
            GATE_CFG_FAIL("class labels must be 1 to %u bytes of UTF-8 "
                          "without control characters", GATE_MAX_LABEL_BYTES);
        for (uint32_t j = 0; j < i; j++)
            if (strcmp(cfg->labels[j], l) == 0)
                GATE_CFG_FAIL("duplicate class label '%s'", l);
    }

    /* Input schema and classifier parameters. */
    const gateSchema *s = &cfg->schema;
    if (cfg->mode == GATE_MODE_CLASSIFIER) {
        if (s->inputs == 0 ||
            (s->inputs & ~(GATE_INPUT_TEXT | GATE_INPUT_VECTOR | GATE_INPUT_FEATURES)))
            GATE_CFG_FAIL("CLASSIFIER mode requires INPUTS with at least one of "
                          "TEXT, VECTOR, FEATURES");
        if (((s->inputs & GATE_INPUT_TEXT) != 0) != (s->text_dim != 0))
            GATE_CFG_FAIL("TEXTDIM requires the TEXT input");
        if (((s->inputs & GATE_INPUT_VECTOR) != 0) != (s->vector_dim != 0))
            GATE_CFG_FAIL("the VECTOR input requires VECTORDIM, and VECTORDIM "
                          "requires the VECTOR input");
        if (((s->inputs & GATE_INPUT_FEATURES) != 0) != (s->feature_dim != 0))
            GATE_CFG_FAIL("FEATUREDIM requires the FEATURES input");
        if (s->text_dim > GATE_MAX_TEXT_DIM)
            GATE_CFG_FAIL("TEXTDIM must be at most %u", GATE_MAX_TEXT_DIM);
        if (s->vector_dim > GATE_MAX_VECTOR_DIM)
            GATE_CFG_FAIL("VECTORDIM must be at most %u", GATE_MAX_VECTOR_DIM);
        if (s->feature_dim > GATE_MAX_FEATURE_DIM)
            GATE_CFG_FAIL("FEATUREDIM must be at most %u", GATE_MAX_FEATURE_DIM);
        if (!(cfg->lr > 0.0 && cfg->lr <= GATE_MAX_LR))
            GATE_CFG_FAIL("LR must be greater than 0 and at most %g", GATE_MAX_LR);
        if (!(cfg->l2 >= 0.0) || !isfinite(cfg->l2))
            GATE_CFG_FAIL("L2 must be a non-negative number");
        if (!(cfg->lr * cfg->l2 < 1.0))
            GATE_CFG_FAIL("LR * L2 must be less than 1");
    } else {
        if (s->inputs || s->text_dim || s->vector_dim || s->feature_dim)
            GATE_CFG_FAIL("SCORE mode does not accept INPUTS, TEXTDIM, VECTORDIM "
                          "or FEATUREDIM");
    }

    /* Versions. */
    if (cfg->teacher == NULL ||
        !gateValidName(cfg->teacher, strlen(cfg->teacher), GATE_MAX_VERSION_BYTES))
        GATE_CFG_FAIL("TEACHER must be 1 to %u bytes of UTF-8 without control "
                      "characters", GATE_MAX_VERSION_BYTES);
    if (cfg->schema_version == NULL ||
        !gateValidName(cfg->schema_version, strlen(cfg->schema_version),
                       GATE_MAX_VERSION_BYTES))
        GATE_CFG_FAIL("SCHEMAVERSION must be 1 to %u bytes of UTF-8 without "
                      "control characters", GATE_MAX_VERSION_BYTES);

    /* Class-conditional policy. */
    if (cfg->class_mode != 0 && cfg->class_mode != 1) GATE_CFG_FAIL("invalid class mode");
    if (cfg->class_mode) {
        if (k == 0) GATE_CFG_FAIL("CLASSALPHA and ALWAYSDEFER require CLASSES");
        if (cfg->class_alpha == NULL || cfg->class_defer == NULL)
            GATE_CFG_FAIL("missing class policy");
        uint32_t allowed = 0;
        for (uint32_t i = 0; i < k; i++) {
            if (cfg->class_defer[i] > 1) GATE_CFG_FAIL("invalid ALWAYSDEFER flag");
            if (!gateInOpenUnit(cfg->class_alpha[i]) || cfg->class_alpha[i] > cfg->alpha)
                GATE_CFG_FAIL("CLASSALPHA for '%s' must be greater than 0 and at "
                              "most ALPHA", cfg->labels[i]);
            if (!cfg->class_defer[i]) allowed++;
        }
        if (allowed == 0) GATE_CFG_FAIL("ALWAYSDEFER cannot cover every class");
    } else if (cfg->class_alpha != NULL || cfg->class_defer != NULL) {
        GATE_CFG_FAIL("class policy without class mode");
    }

    /* Resource limits. */
    if (cfg->max_pending < 1 || cfg->max_pending > GATE_MAX_PENDING_LIMIT)
        GATE_CFG_FAIL("MAXPENDING must be between 1 and %u", GATE_MAX_PENDING_LIMIT);
    if (cfg->observation_ttl_ms < 1 || cfg->observation_ttl_ms > GATE_MAX_TTL_MS)
        GATE_CFG_FAIL("OBSERVATIONTTL must be between 1 and %lld ms",
                      (long long)GATE_MAX_TTL_MS);
    if (cfg->cert_ttl_ms < 1 || cfg->cert_ttl_ms > GATE_MAX_TTL_MS)
        GATE_CFG_FAIL("CERTTTL must be between 1 and %lld ms", (long long)GATE_MAX_TTL_MS);
    if (cfg->max_memory > GATE_MAX_MEMORY_LIMIT)
        GATE_CFG_FAIL("MAXMEMORY must be at most %llu bytes",
                      (unsigned long long)GATE_MAX_MEMORY_LIMIT);

    /* Protocol options. */
    if (!(cfg->drift_margin < 0.0) && !(cfg->drift_margin >= 0.0 && cfg->drift_margin <= 1.0))
        GATE_CFG_FAIL("DRIFTMARGIN must be between 0 and 1");
    if (cfg->on_missing != GATE_ONMISSING_REVOKE && cfg->on_missing != GATE_ONMISSING_ERROR)
        GATE_CFG_FAIL("invalid ONMISSING");
    if (cfg->expand != 0 && cfg->expand != 1) GATE_CFG_FAIL("invalid EXPAND");

    /* The fixed footprint must fit; only pending observations are elastic. */
    uint64_t fixed = gateFixedBytes(cfg);
    if (fixed > cfg->max_memory)
        GATE_CFG_FAIL("MAXMEMORY %llu is too small: this configuration needs at "
                      "least %llu bytes before any pending observation",
                      (unsigned long long)cfg->max_memory, (unsigned long long)fixed);
    return GATE_OK;
}
