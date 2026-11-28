/*
 * Gate data type: unit tests for the linear classifier (gate_model.c).
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * WHERE THE EXPECTATIONS COME FROM. Nothing below is copied from the output
 * of the code under test.
 *
 *   - Literal constants were computed outside this program in 60 digit
 *     decimal arithmetic (Python's decimal module) from the exact binary
 *     values of the inputs: float32 weights and features, double learning
 *     rate, L2 coefficient and targets. They carry 17 significant digits.
 *   - refCompute() is a second implementation of gate_model.h written the
 *     naive way: dense loops over every index, textbook sigmoid, softmax
 *     and cross-entropy. It is only valid for moderate logits, which is all
 *     it is used for.
 *
 * TOLERANCES. Weights are float32, everything else is double, so there are
 * two kinds of expectation:
 *
 *   - CHECK_F32(w, ref): 'ref' is the double value the update formula gives
 *     and the stored weight must be the float32 nearest to it, i.e. within
 *     half a float32 ulp. The factor 0.51 leaves room for the last bit of
 *     two double evaluations of the same formula, which is eight orders of
 *     magnitude below a float32 ulp.
 *   - Probabilities and losses are compared to 1e-15 .. 1e-12 when the
 *     reference starts from the same float32 weights, and to 1e-7 / 1e-8
 *     when it is a number quoted by the spec: those were derived in exact
 *     arithmetic, and a float32 model departs from them in the 9th digit.
 */

#include <fenv.h>
#include <float.h>
#include "testutil.h"
#include "gate_model.h"

#if !defined(NDEBUG)
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#endif

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ------------------------------ Test helpers ----------------------------- */

/* Spacing of float32 numbers at |v|. */
static double ulp32(double v) {
    float f = (float)fabs(v);
    return (double)nextafterf(f, INFINITY) - (double)f;
}

#define CHECK_F32(w, ref) CHECK_NEAR((double)(w), (ref), 0.51 * ulp32(ref))

/* |a - b| <= rel * |b|, for expectations spanning many orders of magnitude.
 * With b == 0 this demands exact equality. */
#define CHECK_REL(a, b, rel) CHECK_NEAR((a), (b), (rel) * fabs(b))

/* Run a helper that contains CHECKs and say which case it was called for
 * when one of them fails (the file:line alone is the same for every case). */
#define CASE(label, call) do { \
    int _failures = gate_test_failures; \
    call; \
    if (gate_test_failures != _failures) printf("    ^ case: %s\n", (label)); \
} while (0)

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (p == NULL) {
        fprintf(stderr, "test_model: out of memory\n");
        abort();
    }
    return p;
}

static int sameBits(const float *a, const float *b, size_t n) {
    return memcmp(a, b, n * sizeof(float)) == 0;
}

static int modelIsZeroed(const gateModel *m) {
    return m->dim == 0 && m->nclasses == 0 && m->nvec == 0 && m->w == NULL &&
           m->b == NULL && m->updates == 0;
}

/* Bit-exact snapshot of everything gateModelLearn() is allowed to modify. */
typedef struct modelSnap {
    float *w, *b;
    size_t nw, nb;
    uint64_t updates;
} modelSnap;

static void snapTake(modelSnap *s, const gateModel *m) {
    s->nw = (size_t)m->nvec * m->dim;
    s->nb = m->nvec;
    s->w = xmalloc(s->nw * sizeof(float));
    s->b = xmalloc(s->nb * sizeof(float));
    memcpy(s->w, m->w, s->nw * sizeof(float));
    memcpy(s->b, m->b, s->nb * sizeof(float));
    s->updates = m->updates;
}

static int snapMatches(const modelSnap *s, const gateModel *m) {
    return sameBits(s->w, m->w, s->nw) && sameBits(s->b, m->b, s->nb) &&
           s->updates == m->updates;
}

static void snapFree(modelSnap *s) {
    free(s->w);
    free(s->b);
}

/* Non-zero value with magnitude in [lo, hi] and a random sign. */
static float randValue(uint64_t *rng, double lo, double hi) {
    double v = lo + (hi - lo) * gate_test_uniform(rng);
    return (float)((gate_test_rand64(rng) & 1) ? v : -v);
}

/* A random coalesced sparse vector: n distinct ascending indices below dim
 * (selection sampling) with non-zero values of magnitude in [lo, hi]. */
static void randSparse(uint64_t *rng, uint32_t dim, uint32_t n, uint32_t *idx, float *val,
                       double lo, double hi)
{
    uint32_t i, chosen = 0;

    for (i = 0; i < dim && chosen < n; i++) {
        if (gate_test_uniform(rng) * (double)(dim - i) < (double)(n - chosen)) {
            idx[chosen] = i;
            val[chosen] = randValue(rng, lo, hi);
            chosen++;
        }
    }
}

/* A random target over K classes: a one-hot label, or a soft distribution
 * with every entry positive. */
static void randTarget(uint64_t *rng, uint32_t K, int soft, double *q) {
    uint32_t k;

    if (!soft) {
        uint32_t label = (uint32_t)(gate_test_rand64(rng) % K);
        for (k = 0; k < K; k++) q[k] = (k == label) ? 1.0 : 0.0;
    } else if (K == 2) {
        q[1] = 0.05 + 0.9 * gate_test_uniform(rng);
        q[0] = 1.0 - q[1];
    } else {
        double sum = 0.0;
        for (k = 0; k < K; k++) {
            q[k] = 0.05 + gate_test_uniform(rng);
            sum += q[k];
        }
        for (k = 0; k < K; k++) q[k] /= sum;
    }
}

/* Give every weight and bias a random value in [-scale, scale]. */
static void randomizeModel(gateModel *m, uint64_t *rng, double scale) {
    size_t i, n = (size_t)m->nvec * m->dim;

    for (i = 0; i < n; i++) m->w[i] = (float)(scale * (2.0 * gate_test_uniform(rng) - 1.0));
    for (i = 0; i < m->nvec; i++) m->b[i] = (float)(scale * (2.0 * gate_test_uniform(rng) - 1.0));
}

static void densify(const gateSparse *x, double *xd, uint32_t dim) {
    uint32_t i;

    for (i = 0; i < dim; i++) xd[i] = 0.0;
    for (i = 0; i < x->n; i++) xd[x->idx[i]] = (double)x->val[i];
}

static int isTouched(const gateSparse *x, uint32_t i) {
    uint32_t j;

    for (j = 0; j < x->n; j++)
        if (x->idx[j] == i) return 1;
    return 0;
}

/* Loss of (x, q) under 'm' without disturbing it. gateModelLearn() reports
 * the loss before its update, so it is run on a throw-away copy. */
static double lossOf(const gateModel *m, const gateSparse *x, const double *q) {
    gateModel c;
    double l = NAN;

    if (gateModelCopy(&c, m) != GATE_OK) return NAN;
    if (gateModelLearn(&c, x, q, 1.0, 0.0, &l) != GATE_OK) l = NAN;
    gateModelFree(&c);
    return l;
}

/* -------------------- Independent dense reference model ------------------ */

static double refClamp(double v) {
    const double lim = (double)GATE_MAX_WEIGHT;
    return v > lim ? lim : (v < -lim ? -lim : v);
}

/* What gate_model.h says one prediction followed by one SGD step must do,
 * computed from the current weights of 'm' and a DENSE input of m->dim
 * doubles (zeros included). New weights are returned as unrounded doubles. */
typedef struct refStep {
    double p[GATE_MAX_CLASSES]; /* class probabilities before the update */
    double loss;                /* cross-entropy before the update */
    int cls;                    /* predicted class, lowest index on ties */
    double score;               /* its probability */
    double *w;                  /* nvec * dim new weights */
    double b[GATE_MAX_CLASSES]; /* new biases */
} refStep;

static void refCompute(refStep *r, const gateModel *m, const double *xd, const double *q,
                       double eta, double lambda)
{
    const uint32_t K = m->nclasses, D = m->dim, nvec = (K == 2) ? 1 : K;
    const double decay = 1.0 - eta * lambda;
    double z[GATE_MAX_CLASSES], err[GATE_MAX_CLASSES];
    uint32_t k, i;

    for (k = 0; k < nvec; k++) {
        double dot = 0.0;
        for (i = 0; i < D; i++) dot += (double)m->w[(size_t)k * D + i] * xd[i];
        z[k] = dot + (double)m->b[k];
    }
    if (K == 2) {
        double p1 = 1.0 / (1.0 + exp(-z[0])), y = q[1];
        r->p[0] = 1.0 - p1;
        r->p[1] = p1;
        r->loss = -(y * log(p1) + (1.0 - y) * log(1.0 - p1));
        err[0] = y - p1;
    } else {
        double sum = 0.0;
        for (k = 0; k < K; k++) {
            r->p[k] = exp(z[k]);
            sum += r->p[k];
        }
        r->loss = 0.0;
        for (k = 0; k < K; k++) {
            r->p[k] /= sum;
            r->loss -= q[k] * log(r->p[k]);
            err[k] = q[k] - r->p[k];
        }
    }
    r->cls = 0;
    for (k = 1; k < K; k++)
        if (r->p[k] > r->p[r->cls]) r->cls = (int)k;
    r->score = r->p[r->cls];

    r->w = xmalloc(sizeof(double) * nvec * D);
    for (k = 0; k < nvec; k++) {
        for (i = 0; i < D; i++)
            r->w[(size_t)k * D + i] =
                refClamp(decay * (double)m->w[(size_t)k * D + i] + eta * err[k] * xd[i]);
        r->b[k] = refClamp((double)m->b[k] + eta * err[k]); /* no decay on the bias */
    }
}

static void refFree(refStep *r) {
    free(r->w);
    r->w = NULL;
}

/* Predict + learn once and hold the model to the reference: probabilities,
 * class, score, loss, and every single weight and bias afterwards. */
static void stepAgainstReference(gateModel *m, const gateSparse *x, const double *q,
                                 double eta, double lambda)
{
    double xd[64], probs[GATE_MAX_CLASSES], score = -1.0, loss = -1.0;
    size_t i, nw = (size_t)m->nvec * m->dim;
    uint32_t k;
    refStep r;
    int cls;

    CHECK(m->dim <= ARRAY_LEN(xd));
    if (m->dim > ARRAY_LEN(xd)) return;
    densify(x, xd, m->dim);
    refCompute(&r, m, xd, q, eta, lambda);

    cls = gateModelPredict(m, x, probs, &score);
    CHECK_EQ_INT(cls, r.cls);
    CHECK_NEAR(score, r.score, 1e-13);
    for (k = 0; k < m->nclasses; k++) CHECK_NEAR(probs[k], r.p[k], 1e-13);

    CHECK(gateModelLearn(m, x, q, eta, lambda, &loss) == GATE_OK);
    CHECK_NEAR(loss, r.loss, 1e-12 * (1.0 + r.loss));
    for (i = 0; i < nw; i++) CHECK_F32(m->w[i], r.w[i]);
    for (k = 0; k < m->nvec; k++) CHECK_F32(m->b[k], r.b[k]);
    refFree(&r);
}

/* ------------------------- gateSigmoid / gateSoftmax --------------------- */

TEST(sigmoid_reference_values) {
    /* sigmoid(z) in 60 digit arithmetic, z being the double the literal
     * denotes. Relative 1e-15: also meaningful for the tiny ones. */
    static const struct { double z, s; } ref[] = {
        {1.0, 0.7310585786300049},     {-1.0, 0.2689414213699951},
        {2.3, 0.90887703898514383},    {-3.7, 0.024127021417669196},
        {0.15, 0.5374298453437496},    {0.05, 0.51249739648421033},
        {0.5, 0.62245933120185459},    {-0.5, 0.37754066879814546},
        {10.0, 0.99995460213129761},   {-10.0, 4.5397868702434395e-05},
        {30.0, 0.99999999999990641},   {-30.0, 9.3576229688392989e-14},
        {36.0, 0.99999999999999978},   {-36.0, 2.3195228302435686e-16},
        {40.0, 1.0},                   {-40.0, 4.2483542552915889e-18},
        {100.0, 1.0},                  {-100.0, 3.7200759760208361e-44},
        {700.0, 1.0},                  {-700.0, 9.8596765437597708e-305},
    };
    size_t i;

    for (i = 0; i < ARRAY_LEN(ref); i++) CHECK_REL(gateSigmoid(ref[i].z), ref[i].s, 1e-15);
    CHECK(gateSigmoid(0.0) == 0.5);
    CHECK(gateSigmoid(-0.0) == 0.5);
    CHECK(isnan(gateSigmoid(NAN))); /* never a plausible probability */
}

TEST(sigmoid_range_symmetry_and_monotonicity) {
    double prev = 0.0, hi, lo;
    int i;

    for (i = -4000; i <= 4000; i++) {
        double z = 0.25 * i; /* -1000 .. 1000 */
        double s = gateSigmoid(z);

        CHECK(s >= 0.0 && s <= 1.0); /* false for a NaN too */
        CHECK(s >= prev);
        /* Where the curve is not saturated a step of 0.25 must be visible. */
        if (i > -4000 && fabs(z) <= 30.0) CHECK(s > prev);
        /* sigmoid(-z) = 1 - sigmoid(z): the positive and the negative
         * branch agree to a few ulps of 1. */
        CHECK_NEAR(s + gateSigmoid(-z), 1.0, 4 * DBL_EPSILON);
        prev = s;
    }

    /* No overflow at the extremes: the results are exact and no floating
     * point exception other than underflow / inexact was raised on the way
     * (the naive 1 / (1 + exp(-z)) computes exp(1000) = Inf here). */
    feclearexcept(FE_ALL_EXCEPT);
    hi = gateSigmoid(1000.0);
    lo = gateSigmoid(-1000.0);
    CHECK(!fetestexcept(FE_OVERFLOW | FE_INVALID | FE_DIVBYZERO));
    CHECK(hi == 1.0);
    CHECK(lo == 0.0);
    CHECK(gateSigmoid(DBL_MAX) == 1.0);
    CHECK(gateSigmoid(-DBL_MAX) == 0.0);
}

TEST(softmax_reference_values) {
    double p[64], lse;
    uint32_t k;

    {
        const double z[] = {1.0, 2.0, 3.0};
        lse = gateSoftmax(z, p, 3);
        CHECK_NEAR(p[0], 0.090030573170380462, 1e-15);
        CHECK_NEAR(p[1], 0.24472847105479764, 1e-15);
        CHECK_NEAR(p[2], 0.6652409557748219, 1e-15);
        CHECK_NEAR(lse, 3.4076059644443801, 2e-15);
    }
    {
        const double z[] = {0.5, -1.25, 3.0, 0.0};
        lse = gateSoftmax(z, p, 4);
        CHECK_NEAR(p[0], 0.071618880371721261, 1e-15);
        CHECK_NEAR(p[1], 0.012445495267699684, 1e-15);
        CHECK_NEAR(p[2], 0.87249657760083876, 1e-15);
        CHECK_NEAR(p[3], 0.043439046759740266, 1e-15);
        CHECK_NEAR(lse, 3.1363965474337765, 2e-15);
    }
    {
        const double z[] = {-2.5, -2.5}; /* an exact tie */
        lse = gateSoftmax(z, p, 2);
        CHECK(p[0] == 0.5 && p[1] == 0.5);
        CHECK_NEAR(lse, -1.8068528194400546, 1e-15);
    }
    {
        const double z[] = {7.25}; /* one class: certain, and lse = z */
        lse = gateSoftmax(z, p, 1);
        CHECK(p[0] == 1.0);
        CHECK(lse == 7.25);
    }
    {
        double z[64];
        for (k = 0; k < 64; k++) z[k] = 0.0;
        lse = gateSoftmax(z, p, 64);
        for (k = 0; k < 64; k++) CHECK(p[k] == 1.0 / 64);
        CHECK_NEAR(lse, 4.1588830833596715, 2e-15); /* log(64) */
    }
    CHECK(gateSoftmax(NULL, NULL, 0) == -INFINITY); /* log of an empty sum */
}

TEST(softmax_sums_to_one_and_ignores_a_common_shift) {
    static const uint32_t sizes[] = {1, 2, 3, 5, 17, 64};
    static const double exact_shifts[] = {1000.0, -1000.0, 1e4, -1e4, 1e15};
    const double z0[] = {1.0, 2.0, 3.0};
    double z[64], zs[64], p[64], ps[64], p0[3];
    uint64_t rng = 0x5eed0001;
    size_t s;
    uint32_t k;
    int trial;

    for (s = 0; s < ARRAY_LEN(sizes); s++) {
        const uint32_t n = sizes[s];

        for (trial = 0; trial < 50; trial++) {
            double sum = 0.0, zmax = -INFINITY, lse, lses;
            const double c = 100.0 * gate_test_uniform(&rng) - 50.0;

            for (k = 0; k < n; k++) {
                z[k] = 16.0 * gate_test_uniform(&rng) - 8.0;
                zs[k] = z[k] + c;
                if (z[k] > zmax) zmax = z[k];
            }
            lse = gateSoftmax(z, p, n);
            lses = gateSoftmax(zs, ps, n);
            for (k = 0; k < n; k++) {
                sum += p[k];
                CHECK(p[k] > 0.0 && p[k] <= 1.0);
                /* The returned value really is the normalizer of p. */
                CHECK_NEAR(p[k], exp(z[k] - lse), 1e-14);
                /* z + c rounds each logit by up to ulp(58)/2 = 3.6e-15. */
                CHECK_NEAR(ps[k], p[k], 1e-13);
            }
            /* n terms, each off by an ulp or two of itself. */
            CHECK_NEAR(sum, 1.0, (n + 2) * DBL_EPSILON);
            CHECK(lse >= zmax && lse <= zmax + log((double)n) + 1e-14);
            CHECK_NEAR(lses, lse + c, 1e-13);
        }
    }

    /* Shifts that add exactly leave the logit differences untouched, so the
     * probabilities must come out bit for bit the same even at 1e15, where
     * a softmax without max subtraction has long since produced NaN. */
    gateSoftmax(z0, p0, 3);
    for (s = 0; s < ARRAY_LEN(exact_shifts); s++) {
        const double c = exact_shifts[s];
        double lses;

        for (k = 0; k < 3; k++) zs[k] = z0[k] + c;
        lses = gateSoftmax(zs, ps, 3);
        for (k = 0; k < 3; k++) CHECK(ps[k] == p0[k]);
        CHECK_NEAR(lses, c + 3.4076059644443801,
                   2e-15 + 2.0 * (nextafter(fabs(c), INFINITY) - fabs(c)));
    }
}

TEST(softmax_huge_logits_do_not_overflow) {
    double p[3], lse;

    feclearexcept(FE_ALL_EXCEPT);
    {
        const double z[] = {1e4, 0.0, -1e4};
        lse = gateSoftmax(z, p, 3);
        CHECK(p[0] == 1.0 && p[1] == 0.0 && p[2] == 0.0);
        CHECK(lse == 1e4);
    }
    {
        const double z[] = {-1e4, 0.0, 1e4};
        lse = gateSoftmax(z, p, 3);
        CHECK(p[0] == 0.0 && p[1] == 0.0 && p[2] == 1.0);
        CHECK(lse == 1e4);
    }
    {
        const double z[] = {0.0, 1e4, 1e4};
        lse = gateSoftmax(z, p, 3);
        CHECK(p[0] == 0.0 && p[1] == 0.5 && p[2] == 0.5);
        CHECK_NEAR(lse, 10000.69314718056, 4e-12); /* 1e4 + log(2) */
    }
    {
        const double z[] = {-1e4, -2e4, -3e4};
        lse = gateSoftmax(z, p, 3);
        CHECK(p[0] == 1.0 && p[1] == 0.0 && p[2] == 0.0);
        CHECK(lse == -1e4);
    }
    {
        const double z[] = {1e300, -1e300, 0.0};
        lse = gateSoftmax(z, p, 3);
        CHECK(p[0] == 1.0 && p[1] == 0.0 && p[2] == 0.0);
        CHECK(lse == 1e300);
    }
    CHECK(!fetestexcept(FE_OVERFLOW | FE_INVALID | FE_DIVBYZERO));
    {
        const double z[] = {0.0, NAN, 1.0}; /* garbage in, NaN out */
        CHECK(isnan(gateSoftmax(z, p, 3)));
    }
}

/* ------------------------------- Zero model ------------------------------ */

TEST(zero_model_binary_predicts_one_half_and_class_zero) {
    uint32_t idx[] = {2, 3};
    float val[] = {1.0f, 1.0f};
    gateSparse x = {2, idx, val}, empty = {0, NULL, NULL};
    double probs[2] = {-1.0, -1.0}, score = -1.0;
    gateModel m;
    uint32_t i;

    memset(&m, 0xA5, sizeof(m)); /* 'm' is fully overwritten */
    CHECK(gateModelInit(&m, 6, 2) == GATE_OK);
    CHECK_EQ_INT(m.dim, 6);
    CHECK_EQ_INT(m.nclasses, 2);
    CHECK_EQ_INT(m.nvec, 1); /* binary: ONE weight vector */
    CHECK_EQ_INT(m.updates, 0);
    for (i = 0; i < 6; i++) CHECK(m.w[i] == 0.0f);
    CHECK(m.b[0] == 0.0f);

    /* p = 0.5 is a tie, and a tie is class 0. */
    CHECK_EQ_INT(gateModelPredict(&m, &x, probs, &score), 0);
    CHECK(score == 0.5);
    CHECK(probs[0] == 0.5 && probs[1] == 0.5);

    score = -1.0;
    CHECK_EQ_INT(gateModelPredict(&m, &empty, NULL, &score), 0);
    CHECK(score == 0.5);
    CHECK_EQ_INT(m.updates, 0); /* predicting is not learning */
    gateModelFree(&m);
}

TEST(zero_model_multiclass_predicts_uniform_and_class_zero) {
    static const uint32_t classes[] = {3, 4, 7, GATE_MAX_CLASSES};
    uint32_t idx[] = {1, 4};
    float val[] = {2.0f, -1.0f};
    gateSparse x = {2, idx, val};
    size_t c;

    for (c = 0; c < ARRAY_LEN(classes); c++) {
        const uint32_t K = classes[c];
        double probs[GATE_MAX_CLASSES], score = -1.0;
        gateModel m;
        uint32_t k;

        CHECK(gateModelInit(&m, 5, K) == GATE_OK);
        CHECK_EQ_INT(m.nvec, K); /* K >= 3: K weight vectors */
        CHECK_EQ_INT(gateModelPredict(&m, &x, probs, &score), 0);
        /* Exactly 1/K, not 1/K minus an ulp: it is the lowest threshold of
         * the classifier grid and the score must not fall below it. */
        CHECK(score == 1.0 / K);
        for (k = 0; k < K; k++) CHECK(probs[k] == 1.0 / K);
        gateModelFree(&m);
    }
}

/* ------------------------- Spec worked examples -------------------------- */

/* Vocabulary of sections 5.2 and 5.3. */
enum { THANKS, APPRECIATE, EXPLAIN, PLEASE, HELP, YOU, VOCAB };

TEST(spec_worked_inference_example) {
    static const float weights[VOCAB] = {-2.0f, -1.5f, 2.0f, 0.8f, 1.8f, 0.3f};
    uint32_t idx1[] = {EXPLAIN, PLEASE};         /* "Please explain" */
    uint32_t idx2[] = {THANKS, APPRECIATE, YOU}; /* "Thanks, appreciate you" */
    float ones[] = {1.0f, 1.0f, 1.0f};
    gateSparse x1 = {2, idx1, ones}, x2 = {3, idx2, ones};
    double probs[2], score;
    gateModel m;

    CHECK(gateModelInit(&m, VOCAB, 2) == GATE_OK);
    memcpy(m.w, weights, sizeof(weights));
    m.b[0] = -0.5f;

    /* z = 2 + 0.8 - 0.5 = 2.3, p = 0.908877 in the spec. The weights 0.8
     * and 0.3 are not float32 numbers (each is off by 1.2e-8), which moves
     * p by up to 0.25 * 1.2e-8: hence 1e-8 against the exact value, and
     * 1e-15 against the value for the float32 weights actually stored. */
    CHECK_EQ_INT(gateModelPredict(&m, &x1, probs, &score), 1);
    CHECK_NEAR(probs[1], 0.90887703898514383, 1e-8);  /* sigmoid(2.3) */
    CHECK_NEAR(probs[1], 0.90887703997243008, 1e-15); /* sigmoid(2.300000011920929) */
    CHECK(score == probs[1]);
    CHECK_NEAR(probs[0] + probs[1], 1.0, DBL_EPSILON);

    /* z = -2 - 1.5 + 0.3 - 0.5 = -3.7, p = 0.024127 in the spec. */
    CHECK_EQ_INT(gateModelPredict(&m, &x2, probs, &score), 0);
    CHECK_NEAR(probs[1], 0.024127021417669196, 1e-8);  /* sigmoid(-3.7) */
    CHECK_NEAR(probs[1], 0.024127021698346383, 1e-15); /* sigmoid(-3.699999988079071) */
    CHECK(score == probs[0]); /* confidence in the predicted class 0 */
    CHECK_NEAR(score, 1.0 - 0.024127021698346383, 1e-15);
    gateModelFree(&m);
}

TEST(spec_worked_training_trace) {
    uint32_t idx1[] = {EXPLAIN, PLEASE};
    uint32_t idx2[] = {THANKS, APPRECIATE, YOU};
    float ones[] = {1.0f, 1.0f, 1.0f};
    gateSparse x1 = {2, idx1, ones}, x2 = {3, idx2, ones};
    const double respond[2] = {0.0, 1.0}, ignore[2] = {1.0, 0.0}; /* y = q[1] */
    double probs[2], score, loss;
    gateModel m;

    CHECK(gateModelInit(&m, VOCAB, 2) == GATE_OK);

    /* "Please explain", y = 1: p = 0.5, error = 0.5, and with eta = 0.1
     * explain = please = bias = 0.05. */
    CHECK_EQ_INT(gateModelPredict(&m, &x1, probs, &score), 0);
    CHECK(probs[1] == 0.5);
    loss = -1.0;
    CHECK(gateModelLearn(&m, &x1, respond, 0.1, 0.0, &loss) == GATE_OK);
    CHECK_NEAR(loss, 0.69314718055994529, 1e-15); /* log(2) */
    CHECK(m.w[EXPLAIN] == 0.05f);
    CHECK(m.w[PLEASE] == 0.05f);
    CHECK(m.b[0] == 0.05f);
    CHECK(m.w[THANKS] == 0.0f && m.w[APPRECIATE] == 0.0f && m.w[HELP] == 0.0f &&
          m.w[YOU] == 0.0f);
    CHECK_EQ_INT(m.updates, 1);

    /* Same example again: z = 0.15, p = 0.537429845 in the spec. 1e-7 is
     * against exact arithmetic; 1e-15 against 0.05f + 0.05f + 0.05f. */
    CHECK_EQ_INT(gateModelPredict(&m, &x1, probs, &score), 1);
    CHECK_NEAR(probs[1], 0.5374298453437496, 1e-7);
    CHECK_NEAR(probs[1], 0.53742984589941167, 1e-15);
    CHECK(score == probs[1]);

    /* "Thanks, appreciate you", y = 0: only the bias is active, so
     * p = sigmoid(0.05) = 0.512497397 in the spec. */
    CHECK_EQ_INT(gateModelPredict(&m, &x2, probs, &score), 1);
    CHECK_NEAR(probs[1], 0.51249739648421033, 1e-7);
    CHECK_NEAR(probs[1], 0.51249739667035854, 1e-15);
    loss = -1.0;
    CHECK(gateModelLearn(&m, &x2, ignore, 0.1, 0.0, &loss) == GATE_OK);
    CHECK_NEAR(loss, 0.71845964839512666, 1e-15); /* -log(1 - p) */

    /* thanks / appreciate / you = -0.051249740 and bias = -0.001249740 in
     * the spec (first line of each pair); the second line is the unrounded
     * result of the same step taken from the stored bias 0.05f. */
    CHECK_NEAR(m.w[THANKS], -0.051249739648421035, 1e-7);
    CHECK_F32(m.w[THANKS], -0.05124973966703586);
    CHECK(m.w[APPRECIATE] == m.w[THANKS] && m.w[YOU] == m.w[THANKS]);
    CHECK_NEAR(m.b[0], -0.0012497396484210345, 1e-7);
    CHECK_F32(m.b[0], -0.0012497389219777999);
    /* explain / please unchanged, help never touched. */
    CHECK(m.w[EXPLAIN] == 0.05f && m.w[PLEASE] == 0.05f);
    CHECK(m.w[HELP] == 0.0f);
    CHECK_EQ_INT(m.updates, 2);
    gateModelFree(&m);
}

/* ------------------------------ Soft targets ----------------------------- */

/* Scenario S1 (binary). z = 0.375 - 0.75 - 1.5 = -1.875 exactly. */
static const float s1_w[8] = {0.25f, -0.5f, 0.125f, 0.75f, -1.0f, 0.5f, 0.0f, -0.25f};
static const float s1_b = 0.375f;
static uint32_t s1_idx[3] = {1, 3, 6};
static float s1_val[3] = {1.5f, -2.0f, 0.5f};
static const double s1_q[2] = {0.7, 0.3}; /* y = 0.3 */

static void s1Load(gateModel *m) {
    CHECK(gateModelInit(m, 8, 2) == GATE_OK);
    memcpy(m->w, s1_w, sizeof(s1_w));
    m->b[0] = s1_b;
}

/* Scenario S2 (3 classes). z = {-0.75, 0.25, 2.25} exactly. */
static const float s2_w[3][6] = {
    {0.5f, -0.25f, 0.0f, 1.0f, 0.125f, -0.75f},
    {-0.5f, 0.75f, 0.25f, -1.0f, 0.0f, 0.5f},
    {0.0f, 0.5f, -0.5f, 0.25f, -0.25f, 1.0f},
};
static const float s2_b[3] = {0.25f, -0.125f, 0.0f};
static uint32_t s2_idx[3] = {0, 2, 5};
static float s2_val[3] = {1.0f, -0.5f, 2.0f};
static const double s2_q[3] = {0.2, 0.5, 0.3};

static void s2Load(gateModel *m) {
    CHECK(gateModelInit(m, 6, 3) == GATE_OK);
    memcpy(m->w, s2_w, sizeof(s2_w));
    memcpy(m->b, s2_b, sizeof(s2_b));
}

TEST(soft_target_binary) {
    /* w' = w + eta (y - p) x with eta = 0.1, p = sigmoid(-1.875). */
    static const double expect_w[8] = {0.25, -0.47494463602967441, 0.125, 0.7165928480395658,
                                       -1.0, 0.5,                  0.008351787990108538, -0.25};
    gateSparse x = {3, s1_idx, s1_val};
    double probs[2], score, loss = -1.0, p, g;
    gateModel m;
    uint32_t i;

    s1Load(&m);
    CHECK_EQ_INT(gateModelPredict(&m, &x, probs, &score), 0);
    CHECK_NEAR(probs[1], 0.13296424019782926, 1e-15);
    CHECK_NEAR(score, 1.0 - 0.13296424019782926, 1e-15);

    CHECK(gateModelLearn(&m, &x, s1_q, 0.1, 0.0, &loss) == GATE_OK);
    CHECK_NEAR(loss, 0.70517505760560151, 1e-15);
    for (i = 0; i < 8; i++) CHECK_F32(m.w[i], expect_w[i]);
    CHECK_F32(m.b[0], 0.3917035759802171);
    /* Untouched weights are not merely close: they were never written. */
    for (i = 0; i < 8; i++)
        if (!isTouched(&x, i)) CHECK(sameBits(&m.w[i], &s1_w[i], 1));
    CHECK_EQ_INT(m.updates, 1);

    /* The same step spelled out from first principles. */
    p = 1.0 / (1.0 + exp(1.875));
    g = 0.1 * (0.3 - p);
    CHECK_F32(m.w[1], -0.5 + g * 1.5);
    CHECK_F32(m.w[3], 0.75 + g * -2.0);
    CHECK_F32(m.w[6], 0.0 + g * 0.5);
    CHECK_F32(m.b[0], 0.375 + g);
    gateModelFree(&m);
}

TEST(soft_target_multiclass) {
    /* W' = W + eta (q_k - p_k) x, eta = 0.1, p = softmax(-0.75, 0.25, 2.25). */
    static const double expect_w[3][6] = {
        {0.51579899338659341, -0.25, -0.0078994966932966985, 1.0, 0.125, -0.71840201322681319},
        {-0.46141951993845942, 0.75, 0.23070975996922971, -1.0, 0.0, 0.57716096012308116},
        {-0.054379473448133951, 0.5, -0.47281026327593301, 0.25, -0.25, 0.89124105310373214},
    };
    static const double expect_b[3] = {0.26579899338659341, -0.086419519938459449,
                                       -0.054379473448133951};
    static const double expect_p[3] = {0.042010066134066049, 0.11419519938459448,
                                       0.84379473448133946};
    gateSparse x = {3, s2_idx, s2_val};
    double probs[3], score, loss = -1.0;
    gateModel m;
    uint32_t k, i;

    s2Load(&m);
    CHECK_EQ_INT(gateModelPredict(&m, &x, probs, &score), 2);
    for (k = 0; k < 3; k++) CHECK_NEAR(probs[k], expect_p[k], 1e-15);
    CHECK(score == probs[2]);

    CHECK(gateModelLearn(&m, &x, s2_q, 0.1, 0.0, &loss) == GATE_OK);
    CHECK_NEAR(loss, 1.7698460195562857, 2e-15);
    for (k = 0; k < 3; k++) {
        for (i = 0; i < 6; i++) {
            CHECK_F32(m.w[k * 6 + i], expect_w[k][i]);
            if (!isTouched(&x, i)) CHECK(sameBits(&m.w[k * 6 + i], &s2_w[k][i], 1));
        }
        CHECK_F32(m.b[k], expect_b[k]);
    }
    gateModelFree(&m);
}

TEST(soft_target_on_a_zero_multiclass_model) {
    /* p = 1/3 everywhere, so row k moves by eta (q_k - 1/3) x: the sign of
     * the step differs per row even though no class is "the label". */
    static const double step[3] = {-0.013333333333333332, 0.016666666666666666,
                                   -0.0033333333333333344};
    uint32_t idx[] = {2, 5};
    float val[] = {1.0f, -2.0f};
    gateSparse x = {2, idx, val};
    const double q[3] = {0.2, 0.5, 0.3};
    double loss = -1.0;
    gateModel m;
    uint32_t k, i;

    CHECK(gateModelInit(&m, 6, 3) == GATE_OK);
    CHECK(gateModelLearn(&m, &x, q, 0.1, 0.0, &loss) == GATE_OK);
    CHECK_NEAR(loss, 1.0986122886681098, 1e-15); /* log(3), whatever q is */
    for (k = 0; k < 3; k++) {
        CHECK_F32(m.w[k * 6 + 2], step[k]);
        CHECK_F32(m.w[k * 6 + 5], -2.0 * step[k]);
        CHECK_F32(m.b[k], step[k]);
        for (i = 0; i < 6; i++)
            if (!isTouched(&x, i)) CHECK(m.w[k * 6 + i] == 0.0f);
    }
    gateModelFree(&m);
}

/* ----------------------------- Gradient check ---------------------------- */

/* The update is w' = w - eta * dF/dw with F = loss + (lambda/2) |w|^2, and
 * it is exactly linear in eta: a small eta buys no accuracy, it only costs
 * float32 resolution. What can be read back from the weights is
 *
 *     implied gradient = -(w' - w) / eta,
 *
 * known to half a float32 ulp of w' divided by eta. Weights are drawn from
 * [-1, 1] and features from [-2, 2], so |w'| < 2, half an ulp is at most
 * 2^-24 = 6e-8, and with eta = 2^-6 the implied gradient is good to 3.8e-6.
 *
 * The other side is a central difference of F over a float32 step of about
 * h = 2^-10 (the two evaluation points are read back from the model, so
 * the step is known exactly). Its truncation error is h^2 F'''/6 < 2e-7
 * for these inputs, and its rounding error is 1e-16 / h = 1e-13.
 *
 * Hence the tolerance of 1e-5, against gradients of order 0.1 .. 1. */
#define GRAD_ETA 0.015625
#define GRAD_H 0.0009765625
#define GRAD_TOL 1e-5

static double objective(const gateModel *m, const gateSparse *x, const double *q, double lambda) {
    size_t i, n = (size_t)m->nvec * m->dim;
    double penalty = 0.0;

    for (i = 0; i < n; i++) penalty += (double)m->w[i] * (double)m->w[i];
    return lossOf(m, x, q) + 0.5 * lambda * penalty; /* the bias is not penalized */
}

/* Central difference of the objective along one stored parameter of 'm'
 * (restored before returning). */
static double fdGradient(gateModel *m, float *param, const gateSparse *x, const double *q,
                         double lambda)
{
    const float orig = *param;
    double hi, lo, fhi, flo;

    *param = (float)((double)orig + GRAD_H);
    hi = (double)*param;
    fhi = objective(m, x, q, lambda);
    *param = (float)((double)orig - GRAD_H);
    lo = (double)*param;
    flo = objective(m, x, q, lambda);
    *param = orig;
    return (fhi - flo) / (hi - lo);
}

static void gradientCheck(uint32_t K, int soft, double lambda, uint64_t seed) {
    enum { DIM = 24, NNZ = 6 };
    uint32_t idx[NNZ];
    float val[NNZ];
    gateSparse x = {NNZ, idx, val};
    double q[GATE_MAX_CLASSES], largest = 0.0;
    gateModel before, after;
    uint64_t rng = seed;
    uint32_t v, i;

    CHECK(gateModelInit(&before, DIM, K) == GATE_OK);
    randomizeModel(&before, &rng, 1.0);
    randSparse(&rng, DIM, NNZ, idx, val, 0.25, 2.0);
    randTarget(&rng, K, soft, q);
    CHECK(gateModelCopy(&after, &before) == GATE_OK);
    CHECK(gateModelLearn(&after, &x, q, GRAD_ETA, lambda, NULL) == GATE_OK);

    for (v = 0; v < before.nvec; v++) {
        const size_t row = (size_t)v * DIM;
        double implied, fd;

        implied = -((double)after.b[v] - (double)before.b[v]) / GRAD_ETA;
        fd = fdGradient(&before, &before.b[v], &x, q, lambda);
        CHECK_NEAR(implied, fd, GRAD_TOL);

        for (i = 0; i < DIM; i++) {
            implied = -((double)after.w[row + i] - (double)before.w[row + i]) / GRAD_ETA;
            fd = fdGradient(&before, &before.w[row + i], &x, q, lambda);
            CHECK_NEAR(implied, fd, GRAD_TOL);
            if (lambda == 0.0 && !isTouched(&x, i)) {
                /* The loss does not depend on this weight at all. */
                CHECK(implied == 0.0);
                CHECK(fd == 0.0);
            }
            if (fabs(fd) > largest) largest = fabs(fd);
        }
    }
    CHECK(largest > 0.05); /* this was not a comparison of zeros */
    gateModelFree(&before);
    gateModelFree(&after);
}

TEST(gradient_check_binary) {
    uint64_t seed;

    for (seed = 1; seed <= 6; seed++) {
        CASE("hard target", gradientCheck(2, 0, 0.0, 0xb1a50000 + seed));
        CASE("soft target", gradientCheck(2, 1, 0.0, 0xb1a51000 + seed));
    }
}

TEST(gradient_check_multiclass) {
    uint64_t seed;

    for (seed = 1; seed <= 4; seed++) {
        CASE("K=3 hard", gradientCheck(3, 0, 0.0, 0x3c1a0000 + seed));
        CASE("K=4 soft", gradientCheck(4, 1, 0.0, 0x4c1a0000 + seed));
    }
    CASE("K=9 soft", gradientCheck(9, 1, 0.0, 0x9c1a0001));
}

TEST(gradient_check_with_l2) {
    /* eta * lambda = 2^-8. Now every weight has a gradient, lambda * w for
     * the ones the input does not touch, and the bias still has none. */
    uint64_t seed;

    for (seed = 1; seed <= 4; seed++) {
        CASE("binary", gradientCheck(2, (int)(seed & 1), 0.25, 0x12b10000 + seed));
        CASE("K=4", gradientCheck(4, (int)(seed & 1), 0.25, 0x12c40000 + seed));
    }
}

/* ---------------------------------- Loss --------------------------------- */

TEST(loss_is_the_cross_entropy_before_the_update) {
    enum { DIM = 20, NNZ = 5 };
    uint32_t idx[NNZ];
    float val[NNZ];
    gateSparse x = {NNZ, idx, val};
    uint64_t rng = 0x5eed0002;
    int trial;

    for (trial = 0; trial < 40; trial++) {
        const uint32_t K = (trial % 2) ? 2 : 3 + (uint32_t)(trial % 5);
        double q[GATE_MAX_CLASSES], probs[GATE_MAX_CLASSES], score, naive = 0.0, loss = -1.0;
        gateModel m;
        uint32_t k;

        CHECK(gateModelInit(&m, DIM, K) == GATE_OK);
        randomizeModel(&m, &rng, 1.0);
        randSparse(&rng, DIM, NNZ, idx, val, 0.25, 2.0);
        randTarget(&rng, K, trial & 2, q);

        /* The textbook formula on the probabilities reported beforehand. */
        gateModelPredict(&m, &x, probs, &score);
        for (k = 0; k < K; k++) naive -= q[k] * log(probs[k]);
        CHECK(gateModelLearn(&m, &x, q, 0.05, 0.0, &loss) == GATE_OK);
        CHECK_NEAR(loss, naive, 1e-13 * (1.0 + naive));
        CHECK(loss > 0.0);
        /* ... and it is the loss BEFORE the update: eta = 0.05 is far below
         * 2 / smoothness for these inputs, so the same example now costs
         * strictly less. */
        CHECK(lossOf(&m, &x, q) < loss);
        gateModelFree(&m);
    }
}

/* Loss reported for a binary model whose logit is exactly b + w * xv, and
 * what the textbook formula makes of the probabilities reported for it. */
static double binaryLossAt(float w, float xv, float b, double y, double *naive) {
    uint32_t idx[1] = {0};
    float val[1];
    gateSparse x = {1, idx, val};
    double q[2], probs[2] = {NAN, NAN}, score, loss = NAN;
    gateModel m;

    val[0] = xv;
    q[0] = 1.0 - y;
    q[1] = y;
    if (gateModelInit(&m, 1, 2) != GATE_OK) return NAN;
    m.w[0] = w;
    m.b[0] = b;
    gateModelPredict(&m, &x, probs, &score);
    *naive = -(y * log(probs[1]) + (1.0 - y) * log(probs[0]));
    if (gateModelLearn(&m, &x, q, 0.1, 0.0, &loss) != GATE_OK) loss = NAN;
    gateModelFree(&m);
    return loss;
}

/* Same for three classes whose logits are exactly the given biases. */
static double multiLossAt(const float *b, const double *q, double *naive) {
    uint32_t idx[1] = {0};
    float val[1] = {1.0f};
    gateSparse x = {1, idx, val};
    double probs[3] = {NAN, NAN, NAN}, score, loss = NAN;
    gateModel m;
    uint32_t k;

    if (gateModelInit(&m, 1, 3) != GATE_OK) return NAN;
    memcpy(m.b, b, 3 * sizeof(float));
    gateModelPredict(&m, &x, probs, &score);
    *naive = 0.0;
    for (k = 0; k < 3; k++) *naive -= q[k] * log(probs[k]);
    if (gateModelLearn(&m, &x, q, 0.1, 0.0, &loss) != GATE_OK) loss = NAN;
    gateModelFree(&m);
    return loss;
}

/* True when the textbook formula is useless: not finite, or wrong by more
 * than 10% (it rounds a tiny loss to 0, or to a multiple of 1.1e-16). */
static int naiveBroke(double naive, double exact) {
    return !(fabs(naive - exact) <= 0.1 * exact);
}

TEST(loss_is_stable_for_extreme_logits) {
    /* Exact values in 60 digit arithmetic. Except where 'naive_ok' is set,
     * -sum_k q_k log(p_k) on double probabilities gives Inf, NaN, 0 or a
     * number that is off by more than 10% for these inputs. (It survives a
     * probability that is tiny, but not one that rounds to 0 or to 1.) */
    static const struct { float w, x, b; double y, loss; int naive_ok; } bin[] = {
        {0.0f, 1.0f, 40.0f, 1.0, 4.2483542552915889e-18, 0},
        {0.0f, 1.0f, 40.0f, 0.0, 40.0, 0},
        {0.0f, 1.0f, 40.0f, 0.25, 30.0, 0},
        {0.0f, 1.0f, -40.0f, 0.0, 4.2483542552915889e-18, 0},
        {0.0f, 1.0f, -40.0f, 1.0, 40.0, 1},
        {0.0f, 1.0f, -40.0f, 0.25, 10.0, 1},
        {0.0f, 1.0f, 36.5f, 1.0, 1.4068617124461467e-16, 0},
        {0.0f, 1.0f, -36.5f, 0.0, 1.4068617124461467e-16, 0},
        {1000.0f, 2.0f, 0.0f, 1.0, 0.0, 0}, /* e^-2000 underflows: exactly 0 */
        {1000.0f, 2.0f, 0.0f, 0.0, 2000.0, 0},
        {1000.0f, 2.0f, 0.0f, 0.3, 1400.0, 0},
        {1000.0f, -2.0f, 0.0f, 0.0, 0.0, 0},
        {1000.0f, -2.0f, 0.0f, 1.0, 2000.0, 0},
        {1000.0f, -2.0f, 0.0f, 0.3, 600.0, 0},
        /* A logit of 1e12. The weight is planted: learned weights saturate
         * at GATE_MAX_WEIGHT, far below it, and no clue is this large. */
        {1e6f, 1e6f, 0.0f, 0.0, 1e12, 0},
        {1e6f, 1e6f, 0.0f, 1.0, 0.0, 0},
        {1e6f, 1e6f, 0.0f, 0.5, 5e11, 0},
    };
    static const struct { float b[3]; double q[3]; double loss; int naive_ok; } multi[] = {
        {{1000.0f, 0.0f, -1000.0f}, {0.0, 0.0, 1.0}, 2000.0, 0},
        {{1000.0f, 0.0f, -1000.0f}, {1.0, 0.0, 0.0}, 0.0, 0},
        {{1000.0f, 0.0f, -1000.0f}, {0.25, 0.25, 0.5}, 1250.0, 0},
        {{50.0f, 0.0f, 0.0f}, {1.0, 0.0, 0.0}, 3.8574996959278356e-22, 0},
        {{50.0f, 0.0f, 0.0f}, {0.0, 1.0, 0.0}, 50.0, 1},
        {{0.0f, 40.0f, 0.0f}, {0.0, 1.0, 0.0}, 8.4967085105831777e-18, 0},
        {{0.0f, 40.0f, 0.0f}, {0.5, 0.0, 0.5}, 40.0, 1},
        {{0.0f, 40.0f, 40.0f}, {0.0, 1.0, 0.0}, 0.69314718055994529, 1},
        {{-1e6f, 1e6f, 0.0f}, {0.5, 0.25, 0.25}, 1250000.0, 0},
    };
    size_t i;

    for (i = 0; i < ARRAY_LEN(bin); i++) {
        double naive = NAN;
        double loss = binaryLossAt(bin[i].w, bin[i].x, bin[i].b, bin[i].y, &naive);

        CHECK(isfinite(loss) && loss >= 0.0);
        CHECK_REL(loss, bin[i].loss, 1e-14);
        CHECK_EQ_INT(naiveBroke(naive, bin[i].loss), !bin[i].naive_ok);
    }
    for (i = 0; i < ARRAY_LEN(multi); i++) {
        double naive = NAN;
        double loss = multiLossAt(multi[i].b, multi[i].q, &naive);

        CHECK(isfinite(loss) && loss >= 0.0);
        CHECK_REL(loss, multi[i].loss, 1e-14);
        CHECK_EQ_INT(naiveBroke(naive, multi[i].loss), !multi[i].naive_ok);
    }
}

TEST(loss_pointer_is_optional) {
    gateSparse x = {3, s2_idx, s2_val};
    double loss = -1.0;
    gateModel a, b;

    s2Load(&a);
    s2Load(&b);
    CHECK(gateModelLearn(&a, &x, s2_q, 0.1, 0.25, &loss) == GATE_OK);
    CHECK(gateModelLearn(&b, &x, s2_q, 0.1, 0.25, NULL) == GATE_OK);
    CHECK(loss > 0.0);
    CHECK(sameBits(a.w, b.w, 18) && sameBits(a.b, b.b, 3));
    CHECK_EQ_INT(a.updates, 1);
    CHECK_EQ_INT(b.updates, 1);
    gateModelFree(&a);
    gateModelFree(&b);
}

/* ---------------------------------- Bias --------------------------------- */

TEST(bias_learns_from_an_empty_input) {
    gateSparse empty = {0, NULL, NULL};
    const double respond[2] = {0.0, 1.0}, third[3] = {0.0, 0.0, 1.0};
    double loss = -1.0, p;
    gateModel m;
    uint32_t i;

    /* Binary: with no features the model is its bias, and it must move. */
    CHECK(gateModelInit(&m, 4, 2) == GATE_OK);
    CHECK(gateModelLearn(&m, &empty, respond, 0.1, 0.0, &loss) == GATE_OK);
    CHECK_NEAR(loss, 0.69314718055994529, 1e-15);
    CHECK(m.b[0] == 0.05f);
    for (i = 0; i < 4; i++) CHECK(m.w[i] == 0.0f);
    CHECK_EQ_INT(m.updates, 1);
    /* Second step from the stored 0.05f: b += eta (1 - sigmoid(b)). */
    p = 1.0 / (1.0 + exp(-(double)0.05f));
    CHECK(gateModelLearn(&m, &empty, respond, 0.1, 0.0, NULL) == GATE_OK);
    CHECK_F32(m.b[0], (double)0.05f + 0.1 * (1.0 - p));
    CHECK_EQ_INT(m.updates, 2);
    gateModelFree(&m);

    /* Multiclass: every bias moves by eta (q_k - 1/3). */
    CHECK(gateModelInit(&m, 4, 3) == GATE_OK);
    CHECK(gateModelLearn(&m, &empty, third, 0.1, 0.0, &loss) == GATE_OK);
    CHECK_NEAR(loss, 1.0986122886681098, 1e-15);
    CHECK_F32(m.b[0], 0.1 * (0.0 - 1.0 / 3.0));
    CHECK_F32(m.b[1], 0.1 * (0.0 - 1.0 / 3.0));
    CHECK_F32(m.b[2], 0.1 * (1.0 - 1.0 / 3.0));
    for (i = 0; i < 12; i++) CHECK(m.w[i] == 0.0f);
    gateModelFree(&m);
}

TEST(bias_is_never_decayed_by_l2) {
    gateSparse x1 = {3, s1_idx, s1_val}, x2 = {3, s2_idx, s2_val}, empty = {0, NULL, NULL};
    const double ignore[2] = {1.0, 0.0};
    gateModel m;
    double p;

    /* S1 with eta * lambda = 0.05. The bias must land where it lands
     * without L2 (0.39170...), not on the decayed 0.37295357598021706. */
    s1Load(&m);
    CHECK(gateModelLearn(&m, &x1, s1_q, 0.1, 0.5, NULL) == GATE_OK);
    CHECK_F32(m.b[0], 0.3917035759802171);
    CHECK(fabs((double)m.b[0] - 0.37295357598021706) > 0.01);
    gateModelFree(&m);

    /* Same for every bias of a multiclass model. */
    s2Load(&m);
    CHECK(gateModelLearn(&m, &x2, s2_q, 0.1, 0.5, NULL) == GATE_OK);
    CHECK_F32(m.b[0], 0.26579899338659341);
    CHECK_F32(m.b[1], -0.086419519938459449);
    CHECK_F32(m.b[2], -0.054379473448133951);
    gateModelFree(&m);

    /* An empty input with L2: the weights shrink, the bias only steps. */
    s1Load(&m);
    p = 1.0 / (1.0 + exp(-0.375));
    CHECK(gateModelLearn(&m, &empty, ignore, 0.1, 0.5, NULL) == GATE_OK);
    CHECK_F32(m.b[0], 0.375 + 0.1 * (0.0 - p));
    CHECK_F32(m.w[4], 0.95 * -1.0);
    gateModelFree(&m);
}

/* ----------------------------------- L2 ---------------------------------- */

TEST(l2_decays_every_weight) {
    /* S1 with eta = 0.1, lambda = 0.5: (1 - 0.05) w for the five untouched
     * weights, (1 - 0.05) w + eta (y - p) x for indices 1, 3 and 6. */
    static const double s1_expect[8] = {
        0.23749999999999999,  -0.44994463602967438, 0.11874999999999999,  0.67909284803956582,
        -0.94999999999999996, 0.47499999999999998,  0.008351787990108538, -0.23749999999999999};
    static const double s2_expect[3][6] = {
        {0.49079899338659339, -0.23749999999999999, -0.0078994966932966985,
         0.94999999999999996, 0.11874999999999999, -0.68090201322681321},
        {-0.4364195199384594, 0.71249999999999991, 0.2182097599692297,
         -0.94999999999999996, 0.0, 0.55216096012308113},
        {-0.054379473448133951, 0.47499999999999998, -0.44781026327593298,
         0.23749999999999999, -0.23749999999999999, 0.84124105310373209},
    };
    gateSparse x1 = {3, s1_idx, s1_val}, x2 = {3, s2_idx, s2_val}, empty = {0, NULL, NULL};
    const double respond[2] = {0.0, 1.0};
    double loss = -1.0;
    gateModel m;
    uint32_t k, i;

    s1Load(&m);
    CHECK(gateModelLearn(&m, &x1, s1_q, 0.1, 0.5, &loss) == GATE_OK);
    CHECK_NEAR(loss, 0.70517505760560151, 1e-15); /* the penalty is not part of it */
    for (i = 0; i < 8; i++) CHECK_F32(m.w[i], s1_expect[i]);
    /* The untouched ones really moved: this is not the lambda = 0 result. */
    CHECK(m.w[0] != s1_w[0] && m.w[4] != s1_w[4] && m.w[7] != s1_w[7]);
    gateModelFree(&m);

    s2Load(&m);
    CHECK(gateModelLearn(&m, &x2, s2_q, 0.1, 0.5, &loss) == GATE_OK);
    CHECK_NEAR(loss, 1.7698460195562857, 2e-15);
    for (k = 0; k < 3; k++)
        for (i = 0; i < 6; i++) CHECK_F32(m.w[k * 6 + i], s2_expect[k][i]);
    gateModelFree(&m);

    /* No feature at all: still every weight decays. */
    s1Load(&m);
    CHECK(gateModelLearn(&m, &empty, respond, 0.1, 0.5, NULL) == GATE_OK);
    for (i = 0; i < 8; i++) CHECK_F32(m.w[i], 0.95 * (double)s1_w[i]);
    gateModelFree(&m);
}

TEST(l2_matches_the_dense_reference) {
    /* Touch the first index, the last one, two neighbours and one in the
     * middle, so every run of untouched weights (before, between, after,
     * and the empty run between neighbours) is exercised. */
    enum { DIM = 40 };
    static const uint32_t classes[] = {2, 3, 6};
    uint32_t idx[] = {0, 7, 8, 21, DIM - 1};
    float val[5];
    gateSparse x = {5, idx, val};
    uint64_t rng = 0x5eed0003;
    size_t c;
    int round, j;

    for (c = 0; c < ARRAY_LEN(classes); c++) {
        double q[GATE_MAX_CLASSES];
        gateModel m;

        CHECK(gateModelInit(&m, DIM, classes[c]) == GATE_OK);
        randomizeModel(&m, &rng, 1.0);
        for (round = 0; round < 5; round++) {
            for (j = 0; j < 5; j++) val[j] = randValue(&rng, 0.25, 2.0);
            randTarget(&rng, classes[c], round & 1, q);
            stepAgainstReference(&m, &x, q, 0.2, 0.35);
        }
        CHECK_EQ_INT(m.updates, 5);
        gateModelFree(&m);
    }
}

TEST(without_l2_untouched_weights_are_bit_identical) {
    enum { DIM = 32, NNZ = 5 };
    static const uint32_t classes[] = {2, 5};
    uint32_t idx[NNZ];
    float val[NNZ];
    gateSparse x = {NNZ, idx, val};
    uint64_t rng = 0x5eed0004;
    size_t c;

    for (c = 0; c < ARRAY_LEN(classes); c++) {
        double q[GATE_MAX_CLASSES];
        gateModel m;
        modelSnap s;
        uint32_t v, i;

        CHECK(gateModelInit(&m, DIM, classes[c]) == GATE_OK);
        randomizeModel(&m, &rng, 1.0);
        randSparse(&rng, DIM, NNZ, idx, val, 0.5, 2.0);
        randTarget(&rng, classes[c], 1, q);
        /* Values an "add zero to everything" update would not preserve
         * bit for bit, or that a stray clamp or decay would alter. */
        for (v = 0; v < m.nvec; v++) {
            uint32_t special = 0;
            for (i = 0; i < DIM && special < 4; i++) {
                static const float odd[4] = {-0.0f, 1e-42f, GATE_MAX_WEIGHT, -123456.789f};
                if (isTouched(&x, i)) continue;
                m.w[(size_t)v * DIM + i] = odd[special++];
            }
        }
        snapTake(&s, &m);
        CHECK(gateModelLearn(&m, &x, q, 0.1, 0.0, NULL) == GATE_OK);
        for (v = 0; v < m.nvec; v++) {
            for (i = 0; i < DIM; i++) {
                const size_t at = (size_t)v * DIM + i;
                if (isTouched(&x, i))
                    CHECK(m.w[at] != s.w[at]); /* and the touched ones did move */
                else
                    CHECK(sameBits(&m.w[at], &s.w[at], 1));
            }
            CHECK(m.b[v] != s.b[v]);
        }
        snapFree(&s);
        gateModelFree(&m);
    }
}

/* ------------------------------- Multiclass ------------------------------ */

TEST(multiclass_rows_share_the_pre_update_probabilities) {
    enum { DIM = 12, NNZ = 5, K = 4 };
    const double eta = 0.5; /* large, so that a wrong order is far off */
    uint32_t idx[NNZ];
    float val[NNZ];
    gateSparse x = {NNZ, idx, val};
    double q[K], xd[DIM], wrong[K * DIM], z[K], p[K], gap = 0.0;
    uint64_t rng = 0x5eed0005;
    gateModel m;
    refStep r;
    uint32_t k, i, j;

    CHECK(gateModelInit(&m, DIM, K) == GATE_OK);
    randomizeModel(&m, &rng, 1.0);
    randSparse(&rng, DIM, NNZ, idx, val, 0.5, 2.0);
    randTarget(&rng, K, 1, q);
    densify(&x, xd, DIM);
    refCompute(&r, &m, xd, q, eta, 0.0);

    /* The tempting wrong implementation: update row 0, recompute the
     * probabilities, update row 1, ... Make sure this test can tell. */
    for (i = 0; i < K * DIM; i++) wrong[i] = (double)m.w[i];
    for (k = 0; k < K; k++) {
        double sum = 0.0;
        for (j = 0; j < K; j++) {
            z[j] = (double)m.b[j];
            for (i = 0; i < DIM; i++) z[j] += wrong[j * DIM + i] * xd[i];
            p[j] = exp(z[j]);
            sum += p[j];
        }
        for (i = 0; i < DIM; i++) wrong[k * DIM + i] += eta * (q[k] - p[k] / sum) * xd[i];
    }
    for (i = 0; i < K * DIM; i++)
        if (fabs(wrong[i] - r.w[i]) > gap) gap = fabs(wrong[i] - r.w[i]);
    CHECK(gap > 1e-3);

    CHECK(gateModelLearn(&m, &x, q, eta, 0.0, NULL) == GATE_OK);
    for (i = 0; i < K * DIM; i++) CHECK_F32(m.w[i], r.w[i]);
    for (k = 0; k < K; k++) CHECK_F32(m.b[k], r.b[k]);
    refFree(&r);
    gateModelFree(&m);
}

TEST(ties_pick_the_lowest_class) {
    static const struct { float b[4]; int cls; } cases[] = {
        {{0.0f, 1.0f, 1.0f, 0.5f}, 1},  /* 1 and 2 tie */
        {{2.0f, 0.0f, 2.0f, 2.0f}, 0},  /* 0, 2 and 3 tie */
        {{-1.0f, -1.0f, 3.0f, 3.0f}, 2},
        {{0.7f, 0.7f, 0.7f, 0.7f}, 0},  /* everything ties */
        {{0.0f, 0.0f, 0.0f, 5.0f}, 3},  /* no tie: not "always the first" */
        {{0.0f, 0.0f, 1.0f, 0.0f}, 2},
        /* Class 1 has the largest logit, but by less than exp() can
         * resolve: the four probabilities are equal, so it is a tie. */
        {{0.0f, 1e-17f, 0.0f, 0.0f}, 0},
    };
    gateSparse empty = {0, NULL, NULL};
    double probs[4], score;
    gateModel m;
    size_t c;
    int k;

    CHECK(gateModelInit(&m, 3, 4) == GATE_OK);
    for (c = 0; c < ARRAY_LEN(cases); c++) {
        memcpy(m.b, cases[c].b, sizeof(cases[c].b));
        CHECK_EQ_INT(gateModelPredict(&m, &empty, probs, &score), cases[c].cls);
        CHECK(score == probs[cases[c].cls]);
        for (k = 0; k < 4; k++) {
            CHECK(probs[k] <= score);                        /* it is the maximum ... */
            if (k < cases[c].cls) CHECK(probs[k] < score);   /* ... and the first one */
        }
    }
    CHECK(probs[0] == 0.25 && probs[1] == 0.25); /* the last case really tied */
    gateModelFree(&m);

    /* Binary: class 1 needs p > 0.5 strictly. */
    CHECK(gateModelInit(&m, 3, 2) == GATE_OK);
    m.b[0] = 1e-17f; /* a positive logit that leaves p at exactly 0.5 */
    CHECK_EQ_INT(gateModelPredict(&m, &empty, probs, &score), 0);
    CHECK(score == 0.5 && probs[1] == 0.5);
    m.b[0] = 1e-3f;
    CHECK_EQ_INT(gateModelPredict(&m, &empty, probs, &score), 1);
    CHECK(score == probs[1] && score > 0.5);
    m.b[0] = -1e-3f;
    CHECK_EQ_INT(gateModelPredict(&m, &empty, probs, &score), 0);
    CHECK(score == probs[0] && score > 0.5);
    gateModelFree(&m);
}

TEST(score_is_the_largest_probability_and_never_below_one_over_k) {
    enum { DIM = 10, NNZ = 4 };
    uint32_t idx[NNZ];
    float val[NNZ];
    gateSparse x = {NNZ, idx, val};
    uint64_t rng = 0x5eed0006;
    int trial;

    for (trial = 0; trial < 300; trial++) {
        const uint32_t K = 2 + (uint32_t)(trial % 9);
        /* From nearly tied logits (1e-9) to saturated ones (30). */
        const double scale = (trial % 3 == 0) ? 1e-9 : ((trial % 3 == 1) ? 1.0 : 30.0);
        double probs[GATE_MAX_CLASSES], score = -1.0, sum = 0.0, best = 0.0;
        gateModel m;
        uint32_t k;
        int cls;

        CHECK(gateModelInit(&m, DIM, K) == GATE_OK);
        randomizeModel(&m, &rng, scale);
        randSparse(&rng, DIM, NNZ, idx, val, 0.5, 2.0);
        cls = gateModelPredict(&m, &x, probs, &score);
        CHECK(cls >= 0 && cls < (int)K);
        for (k = 0; k < K; k++) {
            sum += probs[k];
            if (probs[k] > best) best = probs[k];
        }
        CHECK(score == best);
        CHECK(score == probs[cls]);
        CHECK(score >= 1.0 / K && score <= 1.0);
        CHECK_NEAR(sum, 1.0, (K + 2) * DBL_EPSILON);
        /* The probabilities are optional, the answer is the same. */
        {
            double score2 = -1.0;
            CHECK_EQ_INT(gateModelPredict(&m, &x, NULL, &score2), cls);
            CHECK(score2 == score);
            CHECK_EQ_INT(gateModelPredict(&m, &x, probs, NULL), cls);
        }
        gateModelFree(&m);
    }
}

/* -------------------------- Sparse / dense inputs ------------------------ */

TEST(sparse_input_matches_the_dense_reference) {
    /* The model only ever sees (index, value) pairs. The reference loops
     * over all DIM coordinates, zeros included. Every third input has all
     * DIM coordinates non-zero: a dense vector passed through the sparse
     * interface. */
    enum { DIM = 24 };
    static const struct { uint32_t K; double lambda; } cfg[] = {
        {2, 0.0}, {2, 0.25}, {3, 0.0}, {5, 0.25}, {GATE_MAX_CLASSES, 0.0}};
    uint32_t idx[DIM];
    float val[DIM];
    uint64_t rng = 0x5eed0007;
    size_t c;
    int step;

    for (c = 0; c < ARRAY_LEN(cfg); c++) {
        double q[GATE_MAX_CLASSES];
        gateModel m;

        CHECK(gateModelInit(&m, DIM, cfg[c].K) == GATE_OK);
        randomizeModel(&m, &rng, 0.5);
        for (step = 0; step < 30; step++) {
            gateSparse x = {(step % 3 == 2) ? DIM : 1 + (uint32_t)(step % 6), idx, val};

            randSparse(&rng, DIM, x.n, idx, val, 0.1, 1.5);
            randTarget(&rng, cfg[c].K, step & 1, q);
            stepAgainstReference(&m, &x, q, 0.1, cfg[c].lambda);
        }
        CHECK_EQ_INT(m.updates, 30);
        gateModelFree(&m);
    }
}

/* -------------------------------- Atomicity ------------------------------ */

/* An update that must be refused: GATE_ERR, and not one bit of the weights,
 * the biases, the update counter or the caller's loss variable changes. */
static void expectRejected(gateModel *m, const gateSparse *x, const double *q, double eta,
                           double lambda)
{
    double loss = 12345.0;
    modelSnap s;

    snapTake(&s, m);
    CHECK(gateModelLearn(m, x, q, eta, lambda, &loss) == GATE_ERR);
    CHECK(snapMatches(&s, m));
    CHECK(loss == 12345.0);
    snapFree(&s);
}

TEST(non_finite_update_is_rejected_atomically) {
    enum { DIM = 6 };
    static const uint32_t classes[] = {2, 3};
    static const double lambdas[] = {0.0, 0.25};
    uint32_t idx[] = {1, 3, 5};
    uint64_t rng = 0x5eed0008;
    size_t c, l;

    for (c = 0; c < ARRAY_LEN(classes); c++) {
        const uint32_t K = classes[c];
        const double good_q[3] = {K == 2 ? 0.0 : 0.25, K == 2 ? 1.0 : 0.25, 0.5};
        float good_val[3] = {1.0f, -2.0f, 0.5f};
        gateSparse good = {3, idx, good_val};
        gateModel m;

        double q[3];
        float val[3];
        gateSparse x = {3, idx, val};
        uint32_t v;

        CHECK(gateModelInit(&m, DIM, K) == GATE_OK);
        randomizeModel(&m, &rng, 1.0);
        /* No weight on the last feature, so that it can be made enormous
         * below without saturating the logits. */
        for (v = 0; v < m.nvec; v++) m.w[(size_t)v * DIM + 5] = 0.0f;

        for (l = 0; l < ARRAY_LEN(lambdas); l++) {
            const double lambda = lambdas[l];

            /* A target that is not a number, in every position. */
            memcpy(q, good_q, sizeof(q));
            q[K - 1] = NAN;
            CASE("NaN in the last target", expectRejected(&m, &good, q, 0.1, lambda));
            memcpy(q, good_q, sizeof(q));
            q[0] = NAN;
            CASE("NaN in the first target", expectRejected(&m, &good, q, 0.1, lambda));
            memcpy(q, good_q, sizeof(q));
            q[1] = INFINITY;
            CASE("infinite target", expectRejected(&m, &good, q, 0.1, lambda));
            memcpy(q, good_q, sizeof(q));
            q[0] = -INFINITY;
            CASE("-infinite target", expectRejected(&m, &good, q, 0.1, lambda));

            /* A feature that is not a number: the logits are not finite. */
            memcpy(val, good_val, sizeof(val));
            val[2] = INFINITY;
            CASE("infinite feature", expectRejected(&m, &x, good_q, 0.1, lambda));
            memcpy(val, good_val, sizeof(val));
            val[1] = NAN;
            CASE("NaN feature", expectRejected(&m, &x, good_q, 0.1, lambda));

            /* Learning rates and L2 coefficients outside the contract. */
            CASE("eta NaN", expectRejected(&m, &good, good_q, NAN, lambda));
            CASE("eta Inf", expectRejected(&m, &good, good_q, INFINITY, lambda));
            CASE("eta 0", expectRejected(&m, &good, good_q, 0.0, lambda));
            CASE("eta < 0", expectRejected(&m, &good, good_q, -0.1, lambda));
        }

        /* Moderate logits and probabilities, and a step that overflows only
         * on the LAST feature: eta (q - p) is about 1e299, which times 1
         * and times 2 is still finite (it would be clamped) and times 1e30
         * is not. By the time this is discovered the bias and the first two
         * weights of every vector must not have been written. */
        memcpy(val, good_val, sizeof(val));
        val[2] = 1e30f;
        CASE("overflowing step", expectRejected(&m, &x, good_q, 1e300, 0.0));

        CASE("lambda NaN", expectRejected(&m, &good, good_q, 0.1, NAN));
        CASE("lambda Inf", expectRejected(&m, &good, good_q, 0.1, INFINITY));
        CASE("lambda < 0", expectRejected(&m, &good, good_q, 0.1, -0.5));
        CASE("eta * lambda = 1", expectRejected(&m, &good, good_q, 0.5, 2.0));
        CASE("eta * lambda > 1", expectRejected(&m, &good, good_q, 0.5, 3.0));

        /* After all those refusals the model is alive and well. */
        CHECK_EQ_INT(m.updates, 0);
        CHECK(gateModelLearn(&m, &good, good_q, 0.1, 0.25, NULL) == GATE_OK);
        CHECK_EQ_INT(m.updates, 1);

        /* A weight that is already not finite, in the last vector, at an
         * index the input does not touch. Dense decay would rewrite it as
         * Inf: the update must be refused before any earlier weight or
         * vector has been decayed. Without L2 that weight is neither read
         * nor written, the update goes through and leaves it alone. */
        m.w[(size_t)(m.nvec - 1) * DIM + 4] = INFINITY;
        CASE("corrupt weight, dense decay", expectRejected(&m, &good, good_q, 0.1, 0.25));
        CHECK(gateModelLearn(&m, &good, good_q, 0.1, 0.0, NULL) == GATE_OK);
        CHECK(m.w[(size_t)(m.nvec - 1) * DIM + 4] == INFINITY);
        CHECK_EQ_INT(m.updates, 2);
        /* The same weight under a feature: the logit itself is not finite. */
        m.w[(size_t)(m.nvec - 1) * DIM + 4] = 0.5f;
        m.w[(size_t)(m.nvec - 1) * DIM + 5] = NAN;
        CASE("corrupt weight, touched", expectRejected(&m, &good, good_q, 0.1, 0.0));
        gateModelFree(&m);
    }
}

TEST(overflow_in_the_step_or_the_loss_alone_is_rejected) {
    /* The guarantee covers every intermediate value, so it must hold when
     * only one of them overflows. That takes a target far outside [0, 1]
     * (with a real distribution neither case can arise), which is exactly
     * the kind of input an atomic update must survive. */
    gateSparse x1 = {3, s1_idx, s1_val}, x2 = {3, s2_idx, s2_val}, empty = {0, NULL, NULL};
    double q[3] = {0.0, 0.0, 0.0};
    gateModel m;

    /* Binary, logit 0.375, no feature present: the step 100 * 1e307 is
     * infinite and the bias is the only value it could reach, while the
     * loss, about -1e307 * 0.375, is finite. */
    s1Load(&m);
    q[1] = 1e307;
    CASE("binary, step only", expectRejected(&m, &empty, q, 100.0, 0.0));
    /* Binary, logit -1.875: the loss 1e308 * 1.875 is infinite, while the
     * step 1e-10 * 1e308 and every weight it produces are finite. */
    q[1] = 1e308;
    CASE("binary, loss only", expectRejected(&m, &x1, q, 1e-10, 0.0));
    gateModelFree(&m);

    /* Three classes, same two situations (class 0 has the largest logit
     * for the empty input, the smallest one for x2). */
    s2Load(&m);
    q[1] = 0.0;
    q[0] = 1e307;
    CASE("multiclass, step only", expectRejected(&m, &empty, q, 100.0, 0.0));
    q[0] = 1e308;
    CASE("multiclass, loss only", expectRejected(&m, &x2, q, 1e-10, 0.0));
    gateModelFree(&m);
}

TEST(huge_finite_updates_are_clamped_not_rejected) {
    uint32_t idx[1] = {0};
    float val[1];
    gateSparse x = {1, idx, val};
    const double ignore[2] = {1.0, 0.0}, respond[2] = {0.0, 1.0};
    const double third[3] = {0.0, 0.0, 1.0};
    /* Every weight below is placed relative to the limit, so the test holds
     * for whatever GATE_MAX_WEIGHT is, within the range these scenarios
     * need: 'inside' must remain a weight within the limit, and a step of
     * one half must not be lost in the rounding of a float32 next to it. */
    const float lim = GATE_MAX_WEIGHT;
    const float below = nextafterf(lim, 0.0f); /* the largest float32 inside */
    const float inside = lim - 10.0f;
    double probs[3], score, loss;
    gateModel m;

    CHECK(lim >= 8.0f && lim <= 1048576.0f);

    /* A feature of 1e30 on weights at half the limit: z = lim / 2 * 1e30
     * and the step is 1e29. Enormous, but finite in double, so this is not
     * an error: the weight is clamped and the model stays usable. */
    CHECK(gateModelInit(&m, 3, 2) == GATE_OK);
    m.w[0] = lim / 2;
    m.w[1] = -lim / 2;
    val[0] = 1e30f;
    loss = -1.0;
    CHECK(gateModelLearn(&m, &x, ignore, 0.1, 0.0, &loss) == GATE_OK);
    CHECK_REL(loss, (double)(lim / 2) * (double)1e30f, 1e-15); /* y = 0: the loss is z */
    CHECK(m.w[0] == -lim);
    CHECK(m.w[1] == -lim / 2);
    CHECK_F32(m.b[0], -0.1); /* p = 1, so the bias steps by -eta */
    CHECK_EQ_INT(m.updates, 1);
    /* ... and the other way: z = -lim * 1e30, p = 0, y = 1. */
    CHECK(gateModelLearn(&m, &x, respond, 0.1, 0.0, &loss) == GATE_OK);
    CHECK(m.w[0] == lim);
    CHECK_EQ_INT(gateModelPredict(&m, &x, probs, &score), 1);
    CHECK(score == 1.0 && probs[1] == 1.0 && probs[0] == 0.0);
    gateModelFree(&m);

    /* The bias is clamped too. It starts one float32 inside the limit and
     * w * x = -lim brings the logit to just below 0, so p is one half and a
     * step of eta = 10 (that is 5) overshoots the limit. So does the weight,
     * at -1 + 5 * lim. */
    CHECK(gateModelInit(&m, 3, 2) == GATE_OK);
    m.b[0] = below;
    m.w[0] = -1.0f;
    val[0] = lim;
    CHECK(gateModelLearn(&m, &x, respond, 10.0, 0.0, NULL) == GATE_OK);
    CHECK(m.b[0] == lim);
    CHECK(m.w[0] == lim);
    /* Mirror image, towards the negative limit. */
    m.b[0] = -below;
    m.w[0] = 1.0f;
    CHECK(gateModelLearn(&m, &x, ignore, 10.0, 0.0, NULL) == GATE_OK);
    CHECK(m.b[0] == -lim);
    CHECK(m.w[0] == -lim);
    gateModelFree(&m);

    /* Inside the limit nothing is clamped: z = inside - inside = 0, p = 0.5,
     * and with eta = 1 the bias gains exactly one half while the weight
     * becomes -1 + inside / 2 (54.5 and 26 for a limit of 64). */
    CHECK(gateModelInit(&m, 3, 2) == GATE_OK);
    m.b[0] = inside;
    m.w[0] = -1.0f;
    val[0] = inside;
    CHECK(gateModelLearn(&m, &x, respond, 1.0, 0.0, NULL) == GATE_OK);
    CHECK(m.b[0] == (float)((double)inside + 0.5));
    CHECK(m.w[0] == (float)(-1.0 + 0.5 * (double)inside));
    CHECK(m.b[0] > inside && m.b[0] < lim && fabsf(m.w[0]) < lim);
    gateModelFree(&m);

    /* Multiclass: every vector is clamped on its own side. */
    CHECK(gateModelInit(&m, 3, 3) == GATE_OK);
    val[0] = 1e30f;
    CHECK(gateModelLearn(&m, &x, third, 0.3, 0.0, NULL) == GATE_OK);
    CHECK(m.w[0 * 3 + 0] == -GATE_MAX_WEIGHT); /* 0.3 * (0 - 1/3) * 1e30 */
    CHECK(m.w[1 * 3 + 0] == -GATE_MAX_WEIGHT);
    CHECK(m.w[2 * 3 + 0] == GATE_MAX_WEIGHT);  /* 0.3 * (1 - 1/3) * 1e30 */
    CHECK_EQ_INT(gateModelPredict(&m, &x, probs, &score), 2);
    CHECK(score == 1.0);
    gateModelFree(&m);
}

TEST(predict_reports_non_finite_logits) {
    uint32_t idx[] = {0, 2};
    float val[2];
    gateSparse x = {2, idx, val}, empty = {0, NULL, NULL};
    const double q[3] = {0.0, 1.0, 0.0};
    double probs[GATE_MAX_CLASSES], score;
    gateModel m, dead;
    uint32_t k;

    CHECK(gateModelInit(&m, 3, 2) == GATE_OK);
    m.w[0] = 1.0f;
    m.w[2] = -1.0f;
    m.b[0] = 0.25f;

    val[0] = 1.0f, val[1] = 1.0f; /* sanity: z = 0.25 */
    CHECK_EQ_INT(gateModelPredict(&m, &x, probs, &score), 1);

    val[0] = 1.0f, val[1] = INFINITY; /* z = -Inf */
    probs[0] = probs[1] = score = 7.0;
    CHECK_EQ_INT(gateModelPredict(&m, &x, probs, &score), -1);
    /* Nothing a caller ignoring the -1 could mistake for confidence. */
    CHECK(score == 0.0 && probs[0] == 0.0 && probs[1] == 0.0);

    val[0] = INFINITY, val[1] = INFINITY; /* Inf - Inf */
    CHECK_EQ_INT(gateModelPredict(&m, &x, NULL, &score), -1);
    val[0] = NAN, val[1] = 1.0f;
    CHECK_EQ_INT(gateModelPredict(&m, &x, NULL, &score), -1);
    /* A zero weight does not hide an infinite feature: 0 * Inf is NaN. */
    m.w[2] = 0.0f;
    val[0] = 1.0f, val[1] = INFINITY;
    CHECK_EQ_INT(gateModelPredict(&m, &x, NULL, &score), -1);
    gateModelFree(&m);

    /* Multiclass, with the problem in the LAST vector only: the logits of
     * the first two were already computed and must not leak out. */
    CHECK(gateModelInit(&m, 3, 3) == GATE_OK);
    m.w[2 * 3 + 2] = NAN;
    val[0] = 1.0f, val[1] = 1.0f;
    probs[0] = probs[1] = probs[2] = score = 7.0;
    CHECK_EQ_INT(gateModelPredict(&m, &x, probs, &score), -1);
    CHECK(score == 0.0 && probs[0] == 0.0 && probs[1] == 0.0 && probs[2] == 0.0);
    val[0] = INFINITY;
    m.w[2 * 3 + 2] = 0.0f;
    CHECK_EQ_INT(gateModelPredict(&m, &x, NULL, &score), -1);
    gateModelFree(&m);

    /* A zeroed model has nothing to predict with or to train: both calls
     * fail cleanly. Its class count means nothing, so 'probs' is not
     * written at all. */
    memset(&dead, 0, sizeof(dead));
    score = 7.0;
    for (k = 0; k < GATE_MAX_CLASSES; k++) probs[k] = 7.0;
    CHECK_EQ_INT(gateModelPredict(&dead, &empty, probs, &score), -1);
    CHECK(score == 0.0);
    for (k = 0; k < GATE_MAX_CLASSES; k++) CHECK(probs[k] == 7.0);
    CHECK(gateModelLearn(&dead, &empty, q, 0.1, 0.0, NULL) == GATE_ERR);
    CHECK(modelIsZeroed(&dead));
}

/* --------------------------------- Lifecycle ----------------------------- */

TEST(init_rejects_bad_shapes) {
    static const struct { uint32_t dim, K; } bad[] = {
        {0, 2}, {0, 3}, {0, 0}, {10, 0}, {10, 1}, {10, GATE_MAX_CLASSES + 1},
        {10, 1000}, {10, UINT32_MAX}};
    static const struct { uint32_t dim, K, nvec; } good[] = {
        {1, 2, 1}, {1, 3, 3}, {7, GATE_MAX_CLASSES, GATE_MAX_CLASSES}};
    gateModel m;
    size_t i;

    for (i = 0; i < ARRAY_LEN(bad); i++) {
        memset(&m, 0xA5, sizeof(m));
        CHECK(gateModelInit(&m, bad[i].dim, bad[i].K) == GATE_ERR);
        CHECK(modelIsZeroed(&m)); /* left zeroed, so that ... */
        gateModelFree(&m);        /* ... this is safe */
        CHECK(modelIsZeroed(&m));
    }
    for (i = 0; i < ARRAY_LEN(good); i++) {
        memset(&m, 0xA5, sizeof(m));
        CHECK(gateModelInit(&m, good[i].dim, good[i].K) == GATE_OK);
        CHECK_EQ_INT(m.dim, good[i].dim);
        CHECK_EQ_INT(m.nclasses, good[i].K);
        CHECK_EQ_INT(m.nvec, good[i].nvec);
        CHECK(m.w != NULL && m.b != NULL);
        gateModelFree(&m);
        CHECK(modelIsZeroed(&m));
        gateModelFree(&m); /* twice is fine */
    }
}

TEST(bytes_counts_weights_and_biases) {
    /* The spec's "dense binary FP32 model with 8192 dimensions uses 32 KiB
     * weights", plus one bias; a 3 class model costs three such vectors. */
    CHECK_EQ_INT(gateModelBytes(8192, 2), 32768 + 4);
    CHECK_EQ_INT(gateModelBytes(8192, 3), 3 * (32768 + 4));
    CHECK_EQ_INT(gateModelBytes(1, 2), 8);
    CHECK_EQ_INT(gateModelBytes(5, GATE_MAX_CLASSES), GATE_MAX_CLASSES * 6 * 4);
    /* No model of these shapes can exist. */
    CHECK_EQ_INT(gateModelBytes(0, 2), 0);
    CHECK_EQ_INT(gateModelBytes(10, 1), 0);
    CHECK_EQ_INT(gateModelBytes(10, 0), 0);
    CHECK_EQ_INT(gateModelBytes(10, GATE_MAX_CLASSES + 1), 0);
    /* The largest shape must not wrap around: 64 * (2^32 - 1 + 1) * 4. */
    if (sizeof(size_t) >= 8)
        CHECK((unsigned long long)gateModelBytes(UINT32_MAX, GATE_MAX_CLASSES) ==
              64ull * 4294967296ull * 4ull);
    else
        CHECK(gateModelBytes(UINT32_MAX, GATE_MAX_CLASSES) == SIZE_MAX);
}

TEST(copy_is_deep_and_independent) {
    enum { DIM = 9, NNZ = 3 };
    static const uint32_t classes[] = {2, 4};
    uint32_t idx[NNZ];
    float val[NNZ];
    gateSparse x = {NNZ, idx, val};
    uint64_t rng = 0x5eed0009;
    gateModel zero, zcopy;
    size_t c;
    int i;

    for (c = 0; c < ARRAY_LEN(classes); c++) {
        const uint32_t K = classes[c];
        double q[GATE_MAX_CLASSES], pa[GATE_MAX_CLASSES], pb[GATE_MAX_CLASSES], sa, sb;
        gateModel src, dst;
        modelSnap frozen, source;
        uint32_t k;

        CHECK(gateModelInit(&src, DIM, K) == GATE_OK);
        randomizeModel(&src, &rng, 1.0);
        for (i = 0; i < 3; i++) {
            randSparse(&rng, DIM, NNZ, idx, val, 0.5, 2.0);
            randTarget(&rng, K, 1, q);
            CHECK(gateModelLearn(&src, &x, q, 0.1, 0.0, NULL) == GATE_OK);
        }

        memset(&dst, 0xA5, sizeof(dst)); /* 'dst' may be uninitialized */
        CHECK(gateModelCopy(&dst, &src) == GATE_OK);
        CHECK_EQ_INT(dst.dim, DIM);
        CHECK_EQ_INT(dst.nclasses, K);
        CHECK_EQ_INT(dst.nvec, src.nvec);
        CHECK_EQ_INT(dst.updates, 3); /* the counter travels with the weights */
        CHECK(dst.w != src.w && dst.b != src.b);
        CHECK(sameBits(dst.w, src.w, (size_t)src.nvec * DIM) && sameBits(dst.b, src.b, src.nvec));
        CHECK_EQ_INT(gateModelPredict(&dst, &x, pb, &sb), gateModelPredict(&src, &x, pa, &sa));
        CHECK(sa == sb);

        /* Training the source (with L2, so every weight moves) must not
         * reach the frozen copy ... */
        snapTake(&frozen, &dst);
        for (i = 0; i < 5; i++) {
            randSparse(&rng, DIM, NNZ, idx, val, 0.5, 2.0);
            randTarget(&rng, K, 1, q);
            CHECK(gateModelLearn(&src, &x, q, 0.1, 0.2, NULL) == GATE_OK);
        }
        CHECK(snapMatches(&frozen, &dst));
        CHECK(!sameBits(dst.w, src.w, (size_t)src.nvec * DIM));
        CHECK_EQ_INT(src.updates, 8);
        /* ... and its predictions are still those of the moment it was
         * taken. */
        CHECK_EQ_INT(gateModelPredict(&dst, &x, pb, &sb) >= 0, 1);
        for (k = 0; k < K; k++) CHECK(isfinite(pb[k]));

        /* Nor does training the copy reach the source. */
        snapTake(&source, &src);
        CHECK(gateModelLearn(&dst, &x, q, 0.1, 0.2, NULL) == GATE_OK);
        CHECK(snapMatches(&source, &src));
        CHECK_EQ_INT(dst.updates, 4);

        snapFree(&frozen);
        snapFree(&source);
        gateModelFree(&src); /* two owners, two frees: no double free */
        gateModelFree(&dst);
    }

    /* A zeroed model copies to a zeroed model ... */
    memset(&zero, 0, sizeof(zero));
    memset(&zcopy, 0xA5, sizeof(zcopy));
    CHECK(gateModelCopy(&zcopy, &zero) == GATE_OK);
    CHECK(modelIsZeroed(&zcopy));
    /* ... but a struct that is neither a model nor zeroed is refused. */
    CHECK(gateModelInit(&zero, DIM, 3) == GATE_OK);
    zero.nclasses = 1;
    memset(&zcopy, 0xA5, sizeof(zcopy));
    CHECK(gateModelCopy(&zcopy, &zero) == GATE_ERR);
    CHECK(modelIsZeroed(&zcopy));
    zero.nclasses = 3;
    gateModelFree(&zero);
}

TEST(reset_returns_to_the_initial_model) {
    enum { DIM = 7, NNZ = 3 };
    static const uint32_t classes[] = {2, 3};
    uint32_t idx[NNZ];
    float val[NNZ];
    gateSparse x = {NNZ, idx, val};
    uint64_t rng = 0x5eed000a;
    gateModel dead;
    size_t c, i;
    int step;

    for (c = 0; c < ARRAY_LEN(classes); c++) {
        const uint32_t K = classes[c];
        double q[GATE_MAX_CLASSES], score;
        gateModel m, fresh;
        const float *w, *b;

        CHECK(gateModelInit(&m, DIM, K) == GATE_OK);
        for (step = 0; step < 10; step++) {
            randSparse(&rng, DIM, NNZ, idx, val, 0.5, 2.0);
            randTarget(&rng, K, 0, q);
            CHECK(gateModelLearn(&m, &x, q, 0.3, 0.0, NULL) == GATE_OK);
        }
        CHECK_EQ_INT(m.updates, 10);
        CHECK_EQ_INT(gateModelPredict(&m, &x, NULL, &score) >= 0, 1);
        CHECK(score != 1.0 / K); /* it did learn something */

        w = m.w;
        b = m.b;
        gateModelReset(&m);
        CHECK_EQ_INT(m.updates, 0);
        CHECK_EQ_INT(m.dim, DIM);
        CHECK_EQ_INT(m.nclasses, K);
        CHECK(m.w == w && m.b == b); /* same arrays, emptied in place */
        for (i = 0; i < (size_t)m.nvec * DIM; i++) CHECK(m.w[i] == 0.0f);
        for (i = 0; i < m.nvec; i++) CHECK(m.b[i] == 0.0f);
        CHECK_EQ_INT(gateModelPredict(&m, &x, NULL, &score), 0);
        CHECK(score == 1.0 / K);

        /* From here on it behaves exactly like a new model. */
        CHECK(gateModelInit(&fresh, DIM, K) == GATE_OK);
        CHECK(gateModelLearn(&m, &x, q, 0.3, 0.0, NULL) == GATE_OK);
        CHECK(gateModelLearn(&fresh, &x, q, 0.3, 0.0, NULL) == GATE_OK);
        CHECK(sameBits(m.w, fresh.w, (size_t)m.nvec * DIM) && sameBits(m.b, fresh.b, m.nvec));
        CHECK_EQ_INT(m.updates, 1);
        gateModelFree(&m);
        gateModelFree(&fresh);
    }

    memset(&dead, 0, sizeof(dead));
    gateModelReset(&dead); /* nothing to reset, nothing to crash on */
    CHECK(modelIsZeroed(&dead));
}

/* Allocator hooks that count, and can be told to fail the n-th request. */
static long hook_calls, hook_live, hook_fail_at;
static size_t hook_bytes;

static void *hookMalloc(size_t n) {
    if (++hook_calls == hook_fail_at) return NULL;
    hook_live++;
    hook_bytes += n;
    return malloc(n);
}

static void *hookCalloc(size_t nmemb, size_t size) {
    if (++hook_calls == hook_fail_at) return NULL;
    hook_live++;
    hook_bytes += nmemb * size;
    return calloc(nmemb, size);
}

static void *hookRealloc(void *p, size_t n) {
    if (++hook_calls == hook_fail_at) return NULL;
    if (p == NULL) hook_live++;
    hook_bytes += n;
    return realloc(p, n);
}

static void hookFree(void *p) {
    hook_live--;
    free(p);
}

TEST(allocator_hooks_and_allocation_failures) {
    enum { DIM = 100, K = 3 };
    uint32_t idx[] = {3, 50, 99};
    float val[] = {1.0f, -1.0f, 0.5f};
    gateSparse x = {3, idx, val};
    const double q[K] = {0.0, 1.0, 0.0};
    double probs[K], score;
    gateModel m, c;
    long calls;
    int k, rc = GATE_ERR;

    gateSetAllocator(hookMalloc, hookCalloc, hookRealloc, hookFree);
    hook_calls = hook_live = hook_fail_at = 0;
    hook_bytes = 0;

    /* Everything the model owns comes from the hooks, and gateModelBytes()
     * is exactly what it asked for. */
    CHECK(gateModelInit(&m, DIM, K) == GATE_OK);
    CHECK(hook_live > 0);
    CHECK_EQ_INT(hook_bytes, gateModelBytes(DIM, K));

    /* Predicting and learning do not allocate, with or without L2. */
    calls = hook_calls;
    CHECK_EQ_INT(gateModelPredict(&m, &x, NULL, &score), 0);
    CHECK_EQ_INT(gateModelPredict(&m, &x, probs, &score), 0);
    CHECK(gateModelLearn(&m, &x, q, 0.1, 0.0, NULL) == GATE_OK);
    CHECK(gateModelLearn(&m, &x, q, 0.1, 0.1, NULL) == GATE_OK);
    CHECK_EQ_INT(hook_calls, calls);

    hook_bytes = 0;
    CHECK(gateModelCopy(&c, &m) == GATE_OK);
    CHECK_EQ_INT(hook_bytes, gateModelBytes(DIM, K));
    gateModelFree(&c);
    gateModelFree(&m);
    CHECK_EQ_INT(hook_live, 0); /* nothing leaked, nothing freed twice */

    /* Fail the 1st, 2nd, ... allocation of gateModelInit() until it gets
     * through: every failure must leave 'm' zeroed and nothing behind. */
    for (k = 1; k <= 8; k++) {
        memset(&m, 0xA5, sizeof(m));
        hook_calls = 0;
        hook_fail_at = k;
        rc = gateModelInit(&m, DIM, K);
        hook_fail_at = 0;
        if (rc == GATE_OK) break;
        CHECK(modelIsZeroed(&m));
        CHECK_EQ_INT(hook_live, 0);
    }
    CHECK(rc == GATE_OK);
    CHECK(k > 1); /* at least one failure was really injected */

    /* Same for gateModelCopy(): 'dst' zeroed, the source untouched. */
    CHECK(gateModelLearn(&m, &x, q, 0.1, 0.0, NULL) == GATE_OK);
    for (k = 1; k <= 8; k++) {
        const long live = hook_live;
        modelSnap s;

        snapTake(&s, &m);
        memset(&c, 0xA5, sizeof(c));
        hook_calls = 0;
        hook_fail_at = k;
        rc = gateModelCopy(&c, &m);
        hook_fail_at = 0;
        CHECK(snapMatches(&s, &m));
        snapFree(&s);
        if (rc == GATE_OK) break;
        CHECK(modelIsZeroed(&c));
        CHECK_EQ_INT(hook_live, live);
    }
    CHECK(rc == GATE_OK);
    CHECK(k > 1);
    CHECK_EQ_INT(c.updates, 1);
    gateModelFree(&c);
    gateModelFree(&m);
    CHECK_EQ_INT(hook_live, 0);

    gateSetAllocator(malloc, calloc, realloc, free);
}

/* ------------------------------ It does learn ---------------------------- */

/* A linearly separable problem: 4 active features out of 16 with values of
 * magnitude 0.5 .. 1.5, labelled by a hidden linear teacher (one vector and
 * a sign for 'nteach' = 1, an argmax over 'nteach' vectors otherwise).
 * Examples the teacher separates by less than 0.5 are redrawn, which gives
 * the problem a margin. */
enum { LEARN_DIM = 16, LEARN_NNZ = 4 };

static int drawExample(uint64_t *rng, const double *teacher, uint32_t nteach, uint32_t *idx,
                       float *val)
{
    for (;;) {
        double s[3] = {0.0, 0.0, 0.0}, top, runner = -INFINITY;
        uint32_t k, j, best = 0;

        randSparse(rng, LEARN_DIM, LEARN_NNZ, idx, val, 0.5, 1.5);
        for (k = 0; k < nteach; k++)
            for (j = 0; j < LEARN_NNZ; j++)
                s[k] += teacher[k * LEARN_DIM + idx[j]] * (double)val[j];
        if (nteach == 1) {
            if (fabs(s[0]) < 0.5) continue;
            return s[0] > 0.0;
        }
        for (k = 1; k < nteach; k++)
            if (s[k] > s[best]) best = k;
        top = s[best];
        for (k = 0; k < nteach; k++)
            if (k != best && s[k] > runner) runner = s[k];
        if (top - runner < 0.5) continue;
        return (int)best;
    }
}

static void learnsSeparableProblem(uint32_t K, uint64_t seed) {
    enum { TRAIN = 300, TEST_N = 2000 };
    const uint32_t nteach = (K == 2) ? 1 : K;
    double teacher[3 * LEARN_DIM], first = 0.0, last = 0.0;
    uint32_t idx[LEARN_NNZ], i;
    float val[LEARN_NNZ];
    gateSparse x = {LEARN_NNZ, idx, val};
    uint64_t rng = seed, test_rng;
    int t, correct = 0, untrained = 0;
    gateModel m, blank;

    for (i = 0; i < nteach * LEARN_DIM; i++) teacher[i] = 2.0 * gate_test_uniform(&rng) - 1.0;
    CHECK(gateModelInit(&m, LEARN_DIM, K) == GATE_OK);
    CHECK(gateModelInit(&blank, LEARN_DIM, K) == GATE_OK);

    for (t = 0; t < TRAIN; t++) {
        double q[3] = {0.0, 0.0, 0.0}, loss = -1.0;

        q[drawExample(&rng, teacher, nteach, idx, val)] = 1.0;
        CHECK(gateModelLearn(&m, &x, q, 0.5, 0.0, &loss) == GATE_OK);
        if (t < 50) first += loss / 50;
        if (t >= TRAIN - 50) last += loss / 50;
    }
    CHECK_EQ_INT(m.updates, TRAIN);

    /* Fresh examples, never trained on. */
    test_rng = rng;
    for (t = 0; t < TEST_N; t++) {
        const int label = drawExample(&test_rng, teacher, nteach, idx, val);
        double score;

        if (gateModelPredict(&m, &x, NULL, &score) == label) correct++;
        if (gateModelPredict(&blank, &x, NULL, &score) == label) untrained++;
    }
    CHECK(correct > TEST_N * 95 / 100);
    /* The problem is not degenerate: a model that knows nothing (and so
     * answers class 0) is right about once in K. */
    CHECK(untrained > TEST_N / (int)K * 8 / 10 && untrained < TEST_N / (int)K * 12 / 10);
    /* And the loss went the right way: log(K) at the start, a small
     * fraction of it over the last 50 examples. */
    CHECK(first > 0.5 * log((double)K));
    CHECK(last < 0.5 * first);
    gateModelFree(&m);
    gateModelFree(&blank);
}

TEST(learns_a_separable_binary_problem) {
    CASE("seed 1", learnsSeparableProblem(2, 0x1ea40001));
    CASE("seed 2", learnsSeparableProblem(2, 0x1ea40002));
}

TEST(learns_a_separable_three_class_problem) {
    CASE("seed 1", learnsSeparableProblem(3, 0x1ea40003));
    CASE("seed 2", learnsSeparableProblem(3, 0x1ea40004));
}

/* --------------------------- Debug-build contract ------------------------ */

#if !defined(NDEBUG)
/* Sparse inputs must have strictly increasing indices, all below the model
 * dimension. Release builds (-DNDEBUG) trust the caller; a build with
 * assertions enabled, which is what this Makefile produces, must stop a
 * violation before it turns into an out-of-bounds access.
 *
 * A failed assert() ends in abort(). C11 7.22.4.1 lets a SIGABRT handler
 * not return, and these assertions fire before the model is read or
 * written, so the handler simply jumps back into the test. */
static sigjmp_buf abort_jump;
static double abort_score;

static void onAbort(int sig) {
    (void)sig;
    siglongjmp(abort_jump, 1);
}

/* 1 if the call aborted, 0 if it returned. Deliberately without locals:
 * there is nothing here for the jump to clobber. */
static int callAborts(gateModel *m, const gateSparse *x, const double *q, int learn) {
    if (sigsetjmp(abort_jump, 1) != 0) return 1;
    if (learn)
        gateModelLearn(m, x, q, 0.1, 0.0, NULL);
    else
        gateModelPredict(m, x, NULL, &abort_score);
    return 0;
}

TEST(debug_builds_assert_on_malformed_sparse_input) {
    static const struct { uint32_t n; uint32_t idx[3]; int bad; } cases[] = {
        {3, {1, 4, 7}, 0}, /* well formed */
        {3, {0, 1, 2}, 0},
        {0, {9, 9, 9}, 0}, /* empty: the entries are not looked at */
        {3, {1, 4, 8}, 1}, /* index == dim */
        {1, {9, 0, 0}, 1}, /* index > dim */
        {3, {4, 1, 7}, 1}, /* not ascending */
        {3, {1, 4, 4}, 1}, /* duplicate */
    };
    const double q[2] = {0.0, 1.0};
    float val[3] = {1.0f, 1.0f, 1.0f};
    uint32_t idx[3];
    struct sigaction catch_abort, old_action;
    int saved_stderr, devnull, learn;
    gateModel m;
    modelSnap s;
    size_t c;

    CHECK(gateModelInit(&m, 8, 2) == GATE_OK);

    /* The assertion messages are expected here, not news: keep them out of
     * the test output. (stdout is flushed so that nothing reported so far
     * is lost, should a missing assertion let a bad index crash the test.) */
    fflush(stdout);
    fflush(stderr);
    saved_stderr = dup(STDERR_FILENO);
    devnull = open("/dev/null", O_WRONLY);
    if (saved_stderr >= 0 && devnull >= 0) dup2(devnull, STDERR_FILENO);
    memset(&catch_abort, 0, sizeof(catch_abort));
    catch_abort.sa_handler = onAbort;
    sigemptyset(&catch_abort.sa_mask);
    sigaction(SIGABRT, &catch_abort, &old_action);

    for (learn = 0; learn <= 1; learn++) {
        for (c = 0; c < ARRAY_LEN(cases); c++) {
            gateSparse x = {cases[c].n, idx, val};

            memcpy(idx, cases[c].idx, sizeof(idx));
            snapTake(&s, &m);
            CHECK_EQ_INT(callAborts(&m, &x, q, learn), cases[c].bad);
            /* A refused call has not touched the model. */
            if (cases[c].bad) CHECK(snapMatches(&s, &m));
            snapFree(&s);
        }
    }

    sigaction(SIGABRT, &old_action, NULL);
    fflush(stderr);
    if (saved_stderr >= 0 && devnull >= 0) dup2(saved_stderr, STDERR_FILENO);
    if (saved_stderr >= 0) close(saved_stderr);
    if (devnull >= 0) close(devnull);

    CHECK_EQ_INT(m.updates, 3); /* the three well formed inputs were learned */
    gateModelFree(&m);
}
#endif

int main(void) {
    RUN(sigmoid_reference_values);
    RUN(sigmoid_range_symmetry_and_monotonicity);
    RUN(softmax_reference_values);
    RUN(softmax_sums_to_one_and_ignores_a_common_shift);
    RUN(softmax_huge_logits_do_not_overflow);
    RUN(zero_model_binary_predicts_one_half_and_class_zero);
    RUN(zero_model_multiclass_predicts_uniform_and_class_zero);
    RUN(spec_worked_inference_example);
    RUN(spec_worked_training_trace);
    RUN(soft_target_binary);
    RUN(soft_target_multiclass);
    RUN(soft_target_on_a_zero_multiclass_model);
    RUN(gradient_check_binary);
    RUN(gradient_check_multiclass);
    RUN(gradient_check_with_l2);
    RUN(loss_is_the_cross_entropy_before_the_update);
    RUN(loss_is_stable_for_extreme_logits);
    RUN(loss_pointer_is_optional);
    RUN(bias_learns_from_an_empty_input);
    RUN(bias_is_never_decayed_by_l2);
    RUN(l2_decays_every_weight);
    RUN(l2_matches_the_dense_reference);
    RUN(without_l2_untouched_weights_are_bit_identical);
    RUN(multiclass_rows_share_the_pre_update_probabilities);
    RUN(ties_pick_the_lowest_class);
    RUN(score_is_the_largest_probability_and_never_below_one_over_k);
    RUN(sparse_input_matches_the_dense_reference);
    RUN(non_finite_update_is_rejected_atomically);
    RUN(overflow_in_the_step_or_the_loss_alone_is_rejected);
    RUN(huge_finite_updates_are_clamped_not_rejected);
    RUN(predict_reports_non_finite_logits);
    RUN(init_rejects_bad_shapes);
    RUN(bytes_counts_weights_and_biases);
    RUN(copy_is_deep_and_independent);
    RUN(reset_returns_to_the_initial_model);
    RUN(allocator_hooks_and_allocation_failures);
    RUN(learns_a_separable_binary_problem);
    RUN(learns_a_separable_three_class_problem);
#if !defined(NDEBUG)
    RUN(debug_builds_assert_on_malformed_sparse_input);
#endif
    return TEST_REPORT();
}
