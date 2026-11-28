/*
 * Gate data type: linear classifier.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * The model, its update rule and its numerical contract are specified in
 * gate_model.h. Notes on how this file honours them:
 *
 * NO ALLOCATION ON THE HOT PATHS. gateModelPredict() and gateModelLearn()
 * never allocate: logits and probabilities live in GATE_MAX_CLASSES sized
 * stack buffers, and the atomic update needs no scratch copy of the new
 * weights (see below). Only init and copy touch the allocator.
 *
 * ATOMIC UPDATES IN TWO PASSES. The new value of a weight is a pure function
 * of its old value, the feature and the per-vector step eta * (q - p), all of
 * which are fixed before anything is written. So instead of staging the new
 * weights somewhere, the update evaluates the same expression twice: a first,
 * read-only pass checks that every value it would store is finite, and only
 * then a second pass stores them. Feature indices are strictly increasing,
 * hence distinct: no weight is read after it was written, and the second pass
 * reproduces exactly the values the first one accepted.
 *
 * WHAT CAN BE NON-FINITE. Products of two float32 values are exact in double
 * (24 + 24 significand bits), so a logit is rounded only by its additions,
 * and it cannot overflow: |w| <= GATE_MAX_WEIGHT and |x| <= FLT_MAX bound
 * every term by 3.5e44, and 2^32 terms by 1.5e54. Likewise a step
 * eta * (q - p) * x stays below 1e41 for any eta up to GATE_MAX_LR. The
 * finiteness checks therefore never fire on legitimately large values (those
 * are handled by clamping); they catch NaN / Inf features or targets, a
 * corrupted weight, or an absurd learning rate.
 *
 * DECISIONS ARE TAKEN ON PROBABILITIES. The predicted class is derived from
 * the computed probabilities, not from the logits, exactly as the header
 * states the rule. The two differ when logits are closer than the resolution
 * of exp(): such classes get the same probability, which is a tie, and a tie
 * goes to the lowest class index. This keeps the returned class and the
 * returned score consistent with each other.
 */

#include <assert.h>
#include <math.h>
#include <string.h>
#include "gate_model.h"

/* Atomic updates rely on isfinite() really classifying its argument. Under
 * -ffast-math (-ffinite-math-only) the compiler may assume that NaN and Inf
 * do not exist and fold every check in this file to "finite". */
#if defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__
#error "gate_model.c must not be compiled with -ffast-math / -ffinite-math-only"
#endif

/* -------------------------------- Lifecycle ------------------------------- */

/* Binary logistic regression needs a single weight vector: the probability
 * of class 0 is the complement of the probability of class 1. */
static uint32_t gateModelVectors(uint32_t nclasses) {
    return nclasses == 2 ? 1 : nclasses;
}

static int gateModelShapeIsValid(uint32_t dim, uint32_t nclasses) {
    return dim != 0 && nclasses >= GATE_MIN_CLASSES && nclasses <= GATE_MAX_CLASSES;
}

/* True for a model built by gateModelInit() / gateModelCopy(). Predict and
 * learn check it on every call (a handful of predictable branches) because
 * they size their stack buffers by GATE_MAX_CLASSES: a zeroed or corrupted
 * struct must fail the call instead of overrunning them. */
static int gateModelIsLive(const gateModel *m) {
    return m->w != NULL && m->b != NULL &&
           gateModelShapeIsValid(m->dim, m->nclasses) &&
           m->nvec == gateModelVectors(m->nclasses);
}

size_t gateModelBytes(uint32_t dim, uint32_t nclasses) {
    uint64_t floats;

    if (!gateModelShapeIsValid(dim, nclasses)) return 0;
    /* nvec * (dim weights + 1 bias) is below 2^39 and cannot overflow 64
     * bits. It may not fit a 32 bit size_t: the answer then saturates, so
     * that a caller comparing it against a memory limit rejects the shape. */
    floats = (uint64_t)gateModelVectors(nclasses) * ((uint64_t)dim + 1);
    if (floats > SIZE_MAX / sizeof(float)) return SIZE_MAX;
    return (size_t)floats * sizeof(float);
}

int gateModelInit(gateModel *m, uint32_t dim, uint32_t nclasses) {
    uint32_t nvec;
    float *w, *b;

    memset(m, 0, sizeof(*m));
    if (!gateModelShapeIsValid(dim, nclasses)) return GATE_ERR;
    nvec = gateModelVectors(nclasses);
    /* Only reachable where size_t is 32 bits wide. */
    if ((uint64_t)nvec * dim > SIZE_MAX / sizeof(float)) return GATE_ERR;

    /* calloc: all bits zero is 0.0f, the documented initial model. */
    w = gate_calloc((size_t)nvec * dim, sizeof(float));
    b = gate_calloc(nvec, sizeof(float));
    if (w == NULL || b == NULL) {
        gate_free(w);
        gate_free(b);
        return GATE_ERR;
    }
    m->dim = dim;
    m->nclasses = nclasses;
    m->nvec = nvec;
    m->w = w;
    m->b = b;
    return GATE_OK;
}

void gateModelFree(gateModel *m) {
    if (m == NULL) return;
    gate_free(m->w);
    gate_free(m->b);
    memset(m, 0, sizeof(*m));
}

int gateModelCopy(gateModel *dst, const gateModel *src) {
    if (!gateModelIsLive(src)) {
        /* Not a model. A zeroed struct (never initialized, or already
         * freed) copies to a zeroed struct; anything else is refused. */
        int zeroed = src->w == NULL && src->b == NULL && src->dim == 0 &&
                     src->nclasses == 0 && src->nvec == 0;
        memset(dst, 0, sizeof(*dst));
        return zeroed ? GATE_OK : GATE_ERR;
    }
    if (gateModelInit(dst, src->dim, src->nclasses) != GATE_OK) return GATE_ERR;
    memcpy(dst->w, src->w, (size_t)src->nvec * src->dim * sizeof(float));
    memcpy(dst->b, src->b, src->nvec * sizeof(float));
    dst->updates = src->updates;
    return GATE_OK;
}

void gateModelReset(gateModel *m) {
    if (m->w != NULL) memset(m->w, 0, (size_t)m->nvec * m->dim * sizeof(float));
    if (m->b != NULL) memset(m->b, 0, m->nvec * sizeof(float));
    m->updates = 0;
}

/* ----------------------- Numerically stable helpers ----------------------- */

double gateSigmoid(double z) {
    double e;

    /* exp() only ever sees a non-positive argument: it can underflow to 0,
     * which yields exactly 1 or 0, but it can never overflow. */
    if (z >= 0.0) return 1.0 / (1.0 + exp(-z));
    e = exp(z);
    return e / (1.0 + e);
}

/* Binary cross-entropy -y log(p) - (1-y) log(1-p) as a function of the logit:
 *
 *     max(z,0) - y z + log1p(exp(-|z|))
 *
 * The first two terms are folded into (1-y) z for z >= 0 and -y z for z < 0.
 * That is the same value, but written this way both are non-negative for y
 * in [0,1]: nothing cancels when |z| is large and the target is soft, and
 * the loss can never come out negative. */
static double gateBinaryLoss(double z, double y) {
    double linear = z >= 0.0 ? (1.0 - y) * z : -y * z;
    return linear + log1p(exp(-fabs(z)));
}

/* Softmax of n >= 1 logits. Writes p[k] = softmax(z)[k], stores max_k z_k in
 * '*zmax' and returns log(sum_k exp(z_k - zmax)), a value in [0, log n], so
 * that log-sum-exp(z) = *zmax + result and log p_k = (z_k - *zmax) - result.
 * 'p' may alias 'z'.
 *
 * Subtracting the maximum makes every exponent non-positive, so nothing can
 * overflow, and makes the largest term exactly exp(0) = 1. That term is kept
 * out of the running sum and added back by log1p(): when one class dominates,
 * the others add up to something tiny that 1 + tiny would round away, while
 * log1p(tiny) keeps it to full relative precision. This is what makes the
 * multiclass loss of a confident, correct prediction as accurate as the
 * log1p(exp(-|z|)) of the binary one.
 *
 * It also guarantees p[kmax] = 1 / (1 + others) >= 1/n even after rounding
 * (others <= n - 1, and division is monotone), so the score of a prediction
 * never falls below the first threshold of the classifier grid. */
static double gateSoftmaxShifted(const double *z, double *p, uint32_t n, double *zmax) {
    uint32_t k, kmax = 0;
    double m, others = 0.0, total;

    for (k = 1; k < n; k++)
        if (z[k] > z[kmax]) kmax = k;
    m = z[kmax];
    for (k = 0; k < n; k++) {
        if (k == kmax) continue;
        p[k] = exp(z[k] - m);
        others += p[k];
    }
    total = 1.0 + others;
    p[kmax] = 1.0;
    for (k = 0; k < n; k++) p[k] /= total;
    *zmax = m;
    return log1p(others);
}

double gateSoftmax(const double *z, double *p, uint32_t n) {
    double zmax, shifted;

    if (n == 0) return -INFINITY; /* log of an empty sum */
    shifted = gateSoftmaxShifted(z, p, n, &zmax);
    return zmax + shifted;
}

/* --------------------------------- Predict -------------------------------- */

#ifndef NDEBUG
/* The contract on sparse inputs, re-checked in debug builds only. Strictly
 * increasing is more than tidiness: distinct indices are what lets the two
 * passes of gateModelLearn() see the same old weights. */
static int gateSparseIndicesAreValid(const gateSparse *x, uint32_t dim) {
    uint32_t j;

    for (j = 0; j < x->n; j++) {
        if (x->idx[j] >= dim) return 0;
        if (j > 0 && x->idx[j] <= x->idx[j - 1]) return 0;
    }
    return 1;
}
#endif

/* z = b + sum_i w_i x_i for one weight vector, accumulated in double. */
static double gateLogit(const float *w, float b, const gateSparse *x) {
    double z = (double)b;
    uint32_t j;

    for (j = 0; j < x->n; j++)
        z += (double)w[x->idx[j]] * (double)x->val[j];
    return z;
}

/* A prediction that cannot be trusted: no class, a score of 0 that cannot
 * reach any acceptance threshold, and probabilities that cannot be mistaken
 * for a distribution. 'nclasses' is how many entries of 'probs' to clear. */
static int gatePredictFailed(uint32_t nclasses, double *probs, double *score) {
    uint32_t k;

    if (probs != NULL)
        for (k = 0; k < nclasses; k++) probs[k] = 0.0;
    if (score != NULL) *score = 0.0;
    return -1;
}

int gateModelPredict(const gateModel *m, const gateSparse *x, double *probs, double *score) {
    double z[GATE_MAX_CLASSES], pbuf[GATE_MAX_CLASSES];
    double *p, zmax;
    uint32_t k, best;

    /* The shape of a dead model is not to be trusted: leave 'probs' alone. */
    if (!gateModelIsLive(m)) return gatePredictFailed(0, probs, score);
    assert(x != NULL && gateSparseIndicesAreValid(x, m->dim));

    if (m->nclasses == 2) {
        double z1 = gateLogit(m->w, m->b[0], x), p1;

        if (!isfinite(z1)) return gatePredictFailed(2, probs, score);
        p1 = gateSigmoid(z1);
        if (probs != NULL) {
            probs[0] = 1.0 - p1;
            probs[1] = p1;
        }
        if (p1 > 0.5) {
            if (score != NULL) *score = p1;
            return 1;
        }
        if (score != NULL) *score = 1.0 - p1;
        return 0;
    }

    for (k = 0; k < m->nclasses; k++) {
        z[k] = gateLogit(m->w + (size_t)k * m->dim, m->b[k], x);
        if (!isfinite(z[k])) return gatePredictFailed(m->nclasses, probs, score);
    }
    p = probs != NULL ? probs : pbuf;
    gateSoftmaxShifted(z, p, m->nclasses, &zmax);
    best = 0;
    for (k = 1; k < m->nclasses; k++)
        if (p[k] > p[best]) best = k; /* strict: a tie keeps the lowest index */
    if (score != NULL) *score = p[best];
    return (int)best;
}

/* ---------------------------------- Learn --------------------------------- */

/* The value a weight takes after one step, exactly as the update rule reads,
 * evaluated in double. With lambda = 0 'decay' is exactly 1.0 and decay * w
 * is exact, so the sparse and the dense regime share this one expression and
 * round identically. */
static inline double gateSteppedWeight(float w, double decay, double step, float x) {
    return decay * (double)w + step * (double)x;
}

/* Narrow a new weight or bias to float32. Clamping happens in double, before
 * the conversion: a finite double beyond FLT_MAX has no float32 to convert
 * to, while anything within +-GATE_MAX_WEIGHT always does. */
static inline float gateClampWeight(double v) {
    if (v > (double)GATE_MAX_WEIGHT) return GATE_MAX_WEIGHT;
    if (v < -(double)GATE_MAX_WEIGHT) return -GATE_MAX_WEIGHT;
    return (float)v;
}

/* First pass over one weight vector: returns 1 if every weight the update
 * would store in it is finite. Reads only. */
static int gateVectorStepIsFinite(const float *w, uint32_t dim, const gateSparse *x,
                                  double decay, double step)
{
    uint32_t i, j;

    for (j = 0; j < x->n; j++)
        if (!isfinite(gateSteppedWeight(w[x->idx[j]], decay, step, x->val[j]))) return 0;
    if (decay == 1.0) return 1; /* no other weight is rewritten */

    /* Dense decay rewrites the whole vector. The weights not touched by 'x'
     * become decay * w; the touched ones are looked at once more here, with
     * a value that is not the one they will take, which is harmless. */
    for (i = 0; i < dim; i++)
        if (!isfinite(decay * (double)w[i])) return 0;
    return 1;
}

/* Second pass over one weight vector: store the new weights. Only reached
 * after the first pass accepted every vector of the update. */
static void gateVectorStepCommit(float *w, uint32_t dim, const gateSparse *x,
                                 double decay, double step)
{
    uint32_t i = 0, j;

    if (decay == 1.0) {
        for (j = 0; j < x->n; j++) {
            float *wi = w + x->idx[j];
            *wi = gateClampWeight(gateSteppedWeight(*wi, decay, step, x->val[j]));
        }
        return;
    }

    /* Dense decay. The vector is walked once in index order, decaying the
     * runs between touched entries, so that a touched weight is computed
     * from its old value in double and rounded to float32 a single time,
     * like every other weight. Decaying the whole vector first and adding
     * the step afterwards would round those weights twice. */
    for (j = 0; j < x->n; j++) {
        uint32_t t = x->idx[j];

        for (; i < t; i++) w[i] = gateClampWeight(decay * (double)w[i]);
        w[t] = gateClampWeight(gateSteppedWeight(w[t], decay, step, x->val[j]));
        i = t + 1;
    }
    for (; i < dim; i++) w[i] = gateClampWeight(decay * (double)w[i]);
}

int gateModelLearn(gateModel *m, const gateSparse *x, const double *q,
                   double eta, double lambda, double *loss)
{
    double z[GATE_MAX_CLASSES];    /* logits under the current weights */
    double step[GATE_MAX_CLASSES]; /* eta * (q - p), one per weight vector */
    double decay, l;
    uint32_t dim, nvec, v;

    if (!gateModelIsLive(m)) return GATE_ERR;
    assert(x != NULL && q != NULL && gateSparseIndicesAreValid(x, m->dim));
    dim = m->dim;
    nvec = m->nvec;

    /* The caller guarantees eta > 0 and 0 <= eta * lambda < 1. Anything else
     * (a NaN fails every comparison below, and an infinite eta turns 'decay'
     * into NaN or -Inf) must not reach the weights: a decay outside (0, 1]
     * would grow them or flip their sign. */
    decay = 1.0 - eta * lambda;
    if (!(eta > 0.0) || !(lambda >= 0.0) || !(decay > 0.0)) return GATE_ERR;

    /* Logits, loss and steps, all from the weights as they are now. */
    for (v = 0; v < nvec; v++) {
        z[v] = gateLogit(m->w + (size_t)v * dim, m->b[v], x);
        if (!isfinite(z[v])) return GATE_ERR;
    }
    if (m->nclasses == 2) {
        double y = q[1];

        /* q[0] = 1 - y plays no role in the formulas, but a target that is
         * not a number must not train anything. */
        if (!isfinite(q[0])) return GATE_ERR;
        l = gateBinaryLoss(z[0], y);
        step[0] = eta * (y - gateSigmoid(z[0]));
    } else {
        double zmax, shifted = gateSoftmaxShifted(z, step, nvec, &zmax);

        l = 0.0;
        for (v = 0; v < nvec; v++) {
            /* -q_k log p_k with log p_k = (z_k - zmax) - shifted. Both
             * factors are non-negative, and the term stays finite where p_k
             * itself underflows to 0. 'step' holds p until overwritten. */
            l += q[v] * (shifted + (zmax - z[v]));
            step[v] = eta * (q[v] - step[v]);
        }
    }
    if (!isfinite(l)) return GATE_ERR;

    /* First pass: every value about to be stored must be finite. Nothing
     * has been written yet, so giving up here leaves the model untouched. */
    for (v = 0; v < nvec; v++) {
        if (!isfinite((double)m->b[v] + step[v])) return GATE_ERR;
        if (!gateVectorStepIsFinite(m->w + (size_t)v * dim, dim, x, decay, step[v]))
            return GATE_ERR;
    }

    /* Second pass: commit. Cannot fail. The bias is not regularized. */
    for (v = 0; v < nvec; v++) {
        m->b[v] = gateClampWeight((double)m->b[v] + step[v]);
        gateVectorStepCommit(m->w + (size_t)v * dim, dim, x, decay, step[v]);
    }
    m->updates++;
    if (loss != NULL) *loss = l;
    return GATE_OK;
}
