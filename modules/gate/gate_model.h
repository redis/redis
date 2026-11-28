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
 * Binary logistic regression (K = 2, one weight vector) and multinomial
 * softmax regression (K >= 3, K weight vectors) trained by plain online SGD.
 * No Redis dependency, no statistical logic.
 *
 * BINARY (K = 2). Class 0 is y = 0, class 1 is y = 1.
 *
 *     z = b + sum_i w_i x_i          p = sigmoid(z) = P(class 1 | x)
 *     predict class 1 iff p > 0.5 (a tie picks class 0)
 *     score = max(p, 1 - p)
 *     loss(y) = max(z,0) - y z + log1p(exp(-|z|))        (y in [0,1])
 *     w_i <- (1 - eta lambda) w_i + eta (y - p) x_i       b <- b + eta (y - p)
 *
 * MULTICLASS (K >= 3).
 *
 *     z_k = b_k + W_k . x            p = softmax(z)  (max-subtracted)
 *     predict argmax_k p_k (ties pick the lowest class index)
 *     score = max_k p_k
 *     loss(q) = -sum_k q_k log p_k   (via log-sum-exp)
 *     W_ki <- (1 - eta lambda) W_ki + eta (q_k - p_k) x_i
 *     b_k  <- b_k + eta (q_k - p_k)
 *
 * In both cases every probability used by an update is computed from the
 * weights as they were BEFORE the update, and the bias is never regularized.
 *
 * L2. With lambda = 0 (the default) an update touches only the weights of
 * the non-zero features: O(nnz * vectors). With lambda > 0 the decay factor
 * (1 - eta lambda) is applied to EVERY weight on every update, exactly as
 * the formula says: O(dim * vectors). No lazy approximation is used.
 *
 * Numerics. Weights and biases are float32; dot products, probabilities,
 * losses and updates are computed in double. Weights and biases are clamped
 * to [-GATE_MAX_WEIGHT, GATE_MAX_WEIGHT]. An update that would produce a
 * non-finite value is rejected and leaves the model untouched.
 */

#ifndef GATE_MODEL_H
#define GATE_MODEL_H

#include <stdint.h>
#include "gate_defs.h"

typedef struct gateModel {
    uint32_t dim;      /* D: feature dimension */
    uint32_t nclasses; /* K >= 2 */
    uint32_t nvec;     /* weight vectors: 1 when K == 2, otherwise K */
    float *w;          /* nvec * dim weights, row major: w[v * dim + i] */
    float *b;          /* nvec biases */
    uint64_t updates;  /* successful gateModelLearn() calls */
} gateModel;

/* Heap bytes attributed to a model of this shape (weights + biases). */
size_t gateModelBytes(uint32_t dim, uint32_t nclasses);

/* Allocate an all-zero model. 'm' is fully overwritten. GATE_OK / GATE_ERR
 * (bad shape or allocation failure; 'm' is then left zeroed). */
int gateModelInit(gateModel *m, uint32_t dim, uint32_t nclasses);

/* Release the arrays and zero the struct. Safe on a zeroed model. */
void gateModelFree(gateModel *m);

/* Deep copy 'src' into the (uninitialized or zeroed) 'dst', including the
 * update counter. Used to freeze a candidate. GATE_OK / GATE_ERR. */
int gateModelCopy(gateModel *dst, const gateModel *src);

/* Set every weight and bias to zero and reset the update counter. */
void gateModelReset(gateModel *m);

/* Predict. 'x' must be coalesced with every idx < m->dim (not re-checked in
 * release builds). If 'probs' is not NULL it receives the K class
 * probabilities. '*score' receives the largest probability. Returns the
 * predicted class index (0..K-1), or -1 if the result is not finite. */
int gateModelPredict(const gateModel *m, const gateSparse *x, double *probs, double *score);

/* One SGD step towards the target distribution q[0..K-1] (a hard one-hot
 * label or soft teacher probabilities; the caller guarantees q is finite,
 * non-negative and sums to 1). 'eta' is the learning rate, 'lambda' the L2
 * coefficient (the caller guarantees eta > 0 and 0 <= eta*lambda < 1).
 * If 'loss' is not NULL it receives the loss BEFORE the update.
 * Atomic: returns GATE_ERR and changes nothing if any intermediate or
 * resulting value is not finite. On success increments m->updates. */
int gateModelLearn(gateModel *m, const gateSparse *x, const double *q,
                   double eta, double lambda, double *loss);

/* Numerically stable helpers, exposed for tests. */
double gateSigmoid(double z);
/* p[k] = softmax(z)[k] for k < n; returns log-sum-exp(z). */
double gateSoftmax(const double *z, double *p, uint32_t n);

#endif /* GATE_MODEL_H */
