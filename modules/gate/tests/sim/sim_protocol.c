/*
 * Gate data type: Monte Carlo study of the whole protocol (gate_core.c).
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Usage: sim_protocol [runs [seed]]
 *
 * 'runs' (default 400, about a minute on one core) is the number of gates
 * simulated for every row of every table. Everything is deterministic for a
 * given (runs, seed).
 *
 * Where sim_certify studies the betting test alone, this program drives
 * real gates through gateMaintain() / gateEvaluate() / gatePredict() /
 * gateLearn() the way a client and its teacher do, and prints:
 *
 *   (A) safety: how often a gate ever answers when no threshold of its
 *       scorer deserves it, with prompt, adversarially delayed and lost
 *       answers (must stay below delta);
 *   (B) usefulness: how soon and how much a gate answers for scorers whose
 *       risk falls with the score, and how wrong its answers are;
 *   (C) drift: wrong answers given between the moment a scorer breaks and
 *       the moment the gate stops;
 *   (D) what lost feedback costs;
 *   (E) a classifier gate on a problem it can learn and on pure noise.
 *
 * A SCORER is a distribution of scores and, for each score, the probability
 * that the cheap result disagrees with the teacher. Scores are uniform on
 * [0, top) plus, for some scorers, a share that is exactly 1.0. The risk of
 * every threshold of the grid is computed from that definition by numerical
 * integration, never from simulated requests.
 *
 * TIME. One request per second unless a row says otherwise, and the default
 * feedback window of five minutes: an answer that is never sent holds back
 * the proof behind it for 300 requests.
 *
 * Simulation is evidence, not proof, and all requests here are independent
 * and alike, which is what the guarantee itself assumes. The exit status is
 * non zero if a consistency check fails: gates that answered at a threshold
 * worse than alpha significantly more often than delta allows, an error
 * rate among the answers above alpha, a broken scorer that was not stopped.
 */

#include <time.h>
#include "testutil.h"
#include "gate_core.h"

#define T GATE_THRESHOLDS
#define SIM_NEVER UINT64_MAX
#define SIM_Z95 1.959963984540054
#define SIM_DELTA 0.05
#define SIM_HORIZON 20000u /* requests per run */
#define SIM_TAIL 2000u     /* "at the end": the last requests of a run */
#define SIM_DELAY 250u     /* requests a slow teacher holds an answer back */
#define SIM_FLIP 10000u    /* (C): the request at which the scorer breaks */

/* ------------------------------- utilities ------------------------------- */

static void simDie(const char *why) {
    fprintf(stderr, "sim_protocol: %s\n", why);
    exit(2);
}

static void *simAlloc(size_t nmemb, size_t size) {
    void *p = calloc(nmemb, size);

    if (p == NULL) simDie("out of memory");
    return p;
}

/* An independent generator state for stream 'id' of this run (splitmix64
 * is a bijection of its counter: distinct ids, unrelated streams). */
static uint64_t simStream(uint64_t seed, uint64_t id) {
    uint64_t s = seed ^ (id * 0x9e3779b97f4a7c15ull);

    return gate_test_rand64(&s);
}

/* 95% Wilson score interval for x successes out of n. */
static void simWilson(uint64_t x, uint64_t n, double *lo, double *hi) {
    double z2 = SIM_Z95 * SIM_Z95, dn = (double)n, ph = (double)x / dn;
    double den = 1.0 + z2 / dn;
    double center = (ph + z2 / (2.0 * dn)) / den;
    double half = SIM_Z95 * sqrt(ph * (1.0 - ph) / dn + z2 / (4.0 * dn * dn)) / den;

    *lo = center - half > 0.0 ? center - half : 0.0;
    *hi = center + half < 1.0 ? center + half : 1.0;
    if (x == 0) *lo = 0.0;
}

static int simCmpU64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

    return (x > y) - (x < y);
}

static int simCmpDouble(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;

    return (x > y) - (x < y);
}

/* The q quantile of an ascending array: the smallest value with at least a
 * fraction q of the sample at or below it. SIM_NEVER sorts last. */
static uint64_t simQuantile(const uint64_t *sorted, uint64_t n, double q) {
    uint64_t idx = (uint64_t)ceil(q * (double)n);

    if (idx < 1) idx = 1;
    if (idx > n) idx = n;
    return sorted[idx - 1];
}

static const char *simFmt(char *buf, size_t len, uint64_t v) {
    if (v == SIM_NEVER) snprintf(buf, len, "never");
    else snprintf(buf, len, "%llu", (unsigned long long)v);
    return buf;
}

static double simPct(uint64_t part, uint64_t whole) {
    return whole ? 100.0 * (double)part / (double)whole : 0.0;
}

static double simSeconds(clock_t since) {
    return (double)(clock() - since) / CLOCKS_PER_SEC;
}

/* -------------------------------- scorers -------------------------------- */

enum { SIM_FLAT, SIM_POWER, SIM_BAND, SIM_STEP };

typedef struct simWorld {
    const char *name;
    double atom;       /* share of requests whose score is exactly 1.0 */
    double atom_wrong; /* how often those are wrong */
    double top;        /* the other scores are uniform on [0, top) */
    int shape;         /* how often those are wrong, by score: */
    double a, b;       /* FLAT: a. POWER: a (1 - s/top)^b. BAND: a outside
                        * [0.4, 0.7), never inside. STEP: a below b, 2% from
                        * b up. */
} simWorld;

static double simWrong(const simWorld *w, double s) {
    switch (w->shape) {
    case SIM_POWER: return w->a * pow(1.0 - s / w->top, w->b);
    case SIM_BAND: return s >= 0.4 && s < 0.7 ? 0.0 : w->a;
    case SIM_STEP: return s >= w->b ? 0.02 : w->a;
    default: return w->a;
    }
}

static void simDraw(const simWorld *w, uint64_t *rng, double *score, int *wrong) {
    if (gate_test_uniform(rng) < w->atom) {
        *score = 1.0;
        *wrong = gate_test_uniform(rng) < w->atom_wrong;
    } else {
        *score = w->top * gate_test_uniform(rng);
        *wrong = gate_test_uniform(rng) < simWrong(w, *score);
    }
}

/* Share of the requests with score >= t, and the share of those that are
 * wrong (NaN if there are none): midpoint rule over [t, top). */
static void simRegion(const simWorld *w, double t, double *mass, double *risk) {
    enum { STEPS = 20000 };
    double width = w->top > t ? w->top - t : 0.0, wrong = 0.0;
    int i;

    for (i = 0; i < STEPS && width > 0.0; i++)
        wrong += simWrong(w, t + width * (i + 0.5) / STEPS);
    wrong *= (1.0 - w->atom) * (width / w->top) / STEPS;
    *mass = w->atom + (1.0 - w->atom) * width / w->top;
    *risk = *mass > 0.0 ? (w->atom * w->atom_wrong + wrong) / *mass : NAN;
}

typedef struct simTruth {
    double grid[T], mass[T], risk[T];
    double lowest_risk; /* over all thresholds that see any request */
    int best;           /* lowest threshold with risk <= alpha, -1 if none */
} simTruth;

/* The grid of a score gate and what the scorer really does at each of its
 * thresholds. */
static void simTruthOf(const simWorld *w, double alpha, simTruth *tr) {
    uint32_t t;

    if (gateGridBuild(tr->grid, 0.0, 0.998) != GATE_OK) simDie("cannot build the grid");
    tr->lowest_risk = 1.0;
    tr->best = -1;
    for (t = 0; t < T; t++) {
        simRegion(w, tr->grid[t], &tr->mass[t], &tr->risk[t]);
        if (!(tr->mass[t] > 0.0)) continue;
        if (tr->risk[t] < tr->lowest_risk) tr->lowest_risk = tr->risk[t];
        if (tr->best < 0 && tr->risk[t] <= alpha) tr->best = (int)t;
    }
}

/* -------------------------------- one gate ------------------------------- */

enum { SIM_PROMPT, SIM_ADAPTIVE, SIM_LOSSY };

typedef struct simSetup {
    double alpha, audit;
    const simWorld *world;
    const simTruth *truth;  /* what 'world' does at each threshold */
    const simWorld *broken; /* takes over at request SIM_FLIP; NULL: never */
    int feedback;           /* SIM_PROMPT: every answer at once, in order.
                             * SIM_ADAPTIVE: right answers at once, wrong
                             * ones SIM_DELAY requests later.
                             * SIM_LOSSY: a share 'loss' is never sent. */
    double loss;
    uint64_t outage_from, outage_len; /* requests whose answers are all lost */
    int64_t tick_ms;        /* time between two requests */
} simSetup;

typedef struct simResult {
    uint64_t requests, accepted, wrong; /* wrong: among the accepted */
    uint64_t tail_accepted;             /* accepted among the last SIM_TAIL */
    uint64_t first_accept;              /* requests until the first ACCEPT */
    uint64_t active;                    /* requests asked of an ACTIVE gate */
    uint64_t full, promotions, revocations, missing;
    uint64_t guesses, guesses_wrong;    /* (E): what the snapshots guessed */
    double threshold;                   /* at the end; negative: none */
    int invalid;                        /* answered at a risk above alpha */
    int active_at_flip, stopped;        /* (C) */
    uint64_t flip_wrong, flip_requests; /* (C): from the flip to the stop */
} simResult;

typedef struct simLate {
    uint64_t seq, due;
    int32_t result;
} simLate;

static void simFinish(gate *g, simResult *res) {
    res->full = g->stats.full;
    res->promotions = g->stats.promotions;
    res->revocations = g->stats.revocations;
    res->missing = g->stats.missing;
    res->threshold = gateRowThreshold(g, 0);
    gateFree(g);
}

/* One score gate, one client, one teacher, for SIM_HORIZON requests. */
static void simScoreRun(const simSetup *su, uint64_t seed, simResult *res) {
    simLate late[SIM_DELAY + 1]; /* answers on their way, oldest first */
    uint32_t head = 0, tail = 0;
    uint64_t rng = seed, revoked_before = 0, i;
    gateConfig cfg;
    char err[128];
    gate *g;

    memset(res, 0, sizeof(*res));
    res->first_accept = SIM_NEVER;
    gateConfigInit(&cfg);
    cfg.alpha = su->alpha;
    cfg.delta = SIM_DELTA;
    cfg.audit = su->audit;
    if (gateConfigValidate(&cfg, err, sizeof(err)) != GATE_OK) simDie(err);
    g = gateCreate(&cfg, gate_test_rand64(&rng), gate_test_rand64(&rng), 0);
    if (g == NULL) simDie("out of memory");

    for (i = 0; i < SIM_HORIZON; i++) {
        const simWorld *w = su->broken && i >= SIM_FLIP ? su->broken : su->world;
        int64_t now = (int64_t)i * su->tick_ms;
        gatePredictResult r;
        double score, u;
        gateEval ev;
        int wrong;

        while (head != tail && late[head].due <= i) {
            gateLearn(g, now, g->epoch, late[head].seq, late[head].result);
            head = (head + 1) % (SIM_DELAY + 1);
        }
        gateMaintain(g, now);
        if (su->broken && i == SIM_FLIP) {
            res->active_at_flip = g->serving != NULL;
            revoked_before = g->stats.revocations;
        }
        simDraw(w, &rng, &score, &wrong);
        u = gate_test_uniform(&rng);
        if (gateEvaluate(g, NULL, score, &ev) != GATE_OK ||
            gatePredict(g, now, u, &ev, NULL, &r) != GATE_OK)
            simDie("gatePredict failed");
        res->requests++;
        if (g->serving) res->active++;

        if (r.action == GATE_ACTION_ACCEPT) {
            res->accepted++;
            res->wrong += (uint64_t)wrong;
            if (res->first_accept == SIM_NEVER) res->first_accept = i + 1;
            if (i + SIM_TAIL >= SIM_HORIZON) res->tail_accepted++;
            if (su->broken && i >= SIM_FLIP) res->flip_wrong += (uint64_t)wrong;
        } else if (r.seq != 0) {
            int lost = su->feedback == SIM_LOSSY && gate_test_uniform(&rng) < su->loss;

            if (i >= su->outage_from && i - su->outage_from < su->outage_len) lost = 1;
            if (lost) {
                /* The teacher's answer never reaches the gate. */
            } else if (su->feedback == SIM_ADAPTIVE && wrong) {
                late[tail].seq = r.seq;
                late[tail].result = 0;
                late[tail].due = i + SIM_DELAY;
                tail = (tail + 1) % (SIM_DELAY + 1);
            } else {
                gateLearn(g, now, g->epoch, r.seq, !wrong);
            }
        }
        /* The policy only moves down, so looking once per request sees
         * every threshold the gate ever answered at. */
        if (w == su->world && g->serving && g->serving->policy[0] >= 0 &&
            !(su->truth->risk[g->serving->policy[0]] <= su->alpha))
            res->invalid = 1;
        if (su->broken && i >= SIM_FLIP && g->stats.revocations > revoked_before) {
            res->stopped = 1;
            res->flip_requests = i - SIM_FLIP + 1;
            break;
        }
    }
    simFinish(g, res);
}

/* ------------------------------ a classifier -----------------------------
 * 64 words, one bucket each: 16 speak for label 0, 16 for label 1, 32 for
 * neither. A request has a topic (0 or 1) and four words, each one a word
 * of its topic with probability 0.55, of the other topic with 0.10, a
 * neutral one with 0.35. In the learnable problem the teacher's label is
 * the topic: about 11% of the requests cannot be told apart even by the
 * best possible reader (ties and misleading words). In the noise problem
 * the teacher flips a coin. */

#define SIM_WORDS 4

static void simClues(uint64_t *rng, int topic, uint32_t *idx, float *val, gateSparse *x) {
    uint32_t words[SIM_WORDS], i, j, n = 0;

    for (i = 0; i < SIM_WORDS; i++) {
        double r = gate_test_uniform(rng);
        uint32_t pick = (uint32_t)(gate_test_rand64(rng) % 16);

        if (r < 0.55) words[i] = (uint32_t)topic * 16 + pick;
        else if (r < 0.65) words[i] = (uint32_t)(1 - topic) * 16 + pick;
        else words[i] = 32 + (uint32_t)(gate_test_rand64(rng) % 32);
    }
    /* Coalesce: ascending buckets, a repeated word counts twice. */
    for (i = 1; i < SIM_WORDS; i++) {
        uint32_t w = words[i];

        for (j = i; j > 0 && words[j - 1] > w; j--) words[j] = words[j - 1];
        words[j] = w;
    }
    for (i = 0; i < SIM_WORDS; i++) {
        if (n && idx[n - 1] == words[i] * 97 + 11) {
            val[n - 1] += 1.0f;
        } else {
            idx[n] = words[i] * 97 + 11;
            val[n++] = 1.0f;
        }
    }
    x->n = n;
    x->idx = idx;
    x->val = val;
}

static void simClassRun(int learnable, double alpha, uint64_t seed, simResult *res) {
    static const char *const labels[] = {"no", "yes"};
    static const size_t lens[] = {2, 3};
    uint64_t rng = seed, i;
    gateConfig cfg;
    char err[128];
    gate *g;

    memset(res, 0, sizeof(*res));
    res->first_accept = SIM_NEVER;
    gateConfigInit(&cfg);
    cfg.kind = GATE_KIND_CLASSIFIER;
    cfg.alpha = alpha;
    cfg.delta = SIM_DELTA;
    if (gateConfigSetClasses(&cfg, labels, lens, 2) != GATE_OK) simDie("out of memory");
    if (gateConfigValidate(&cfg, err, sizeof(err)) != GATE_OK) simDie(err);
    g = gateCreate(&cfg, gate_test_rand64(&rng), gate_test_rand64(&rng), 0);
    if (g == NULL) simDie("out of memory");
    gateConfigFree(&cfg);

    for (i = 0; i < SIM_HORIZON; i++) {
        int64_t now = (int64_t)i * 1000;
        int topic = (int)(gate_test_rand64(&rng) & 1);
        int32_t label = learnable ? topic : (int32_t)(gate_test_rand64(&rng) & 1);
        uint32_t idx[SIM_WORDS];
        float val[SIM_WORDS];
        gateSparse x, owned = {0, NULL, NULL};
        gatePredictResult r;
        gateEval ev;
        double u;

        simClues(&rng, topic, idx, val, &x);
        u = gate_test_uniform(&rng);
        gateMaintain(g, now);
        if (gateEvaluate(g, &x, 0.0, &ev) != GATE_OK) simDie("gateEvaluate failed");
        /* The gate keeps the clues of the requests it records. */
        if (gatePredictNeedsFeatures(g, u, &ev) && gateSparseCopy(&owned, &x) != GATE_OK)
            simDie("out of memory");
        if (gatePredict(g, now, u, &ev, owned.n ? &owned : NULL, &r) != GATE_OK)
            simDie("gatePredict failed");
        gateSparseFree(&owned);
        res->requests++;
        if (g->serving) res->active++;
        if (ev.has_serving || ev.has_candidate) {
            res->guesses++;
            res->guesses_wrong +=
                (ev.has_serving ? ev.serving.label : ev.candidate.label) != label;
        }

        if (r.action == GATE_ACTION_ACCEPT) {
            res->accepted++;
            res->wrong += r.label != label;
            if (res->first_accept == SIM_NEVER) res->first_accept = i + 1;
            if (i + SIM_TAIL >= SIM_HORIZON) res->tail_accepted++;
        } else if (r.seq != 0) {
            gateLearn(g, now, g->epoch, r.seq, label);
        }
    }
    simFinish(g, res);
}

/* ------------------------------ one table row ---------------------------- */

typedef struct simRow {
    uint64_t runs, ever_active, ever_accepted, invalid;
    uint64_t requests, accepted, wrong, tail_accepted, active;
    uint64_t full, revocations, missing, guesses, guesses_wrong;
    uint64_t active_at_flip, stopped;
    uint64_t *first;      /* per run */
    uint64_t *flip_wrong; /* per run that was active at the flip */
    uint64_t *flip_requests;
    double *threshold;    /* per run that answers at the end */
    uint64_t nthreshold;
} simRow;

static void simRowInit(simRow *row, uint64_t runs) {
    memset(row, 0, sizeof(*row));
    row->first = simAlloc(runs, sizeof(uint64_t));
    row->flip_wrong = simAlloc(runs, sizeof(uint64_t));
    row->flip_requests = simAlloc(runs, sizeof(uint64_t));
    row->threshold = simAlloc(runs, sizeof(double));
}

static void simRowAdd(simRow *row, const simResult *res) {
    row->first[row->runs++] = res->first_accept;
    row->ever_active += res->promotions > 0;
    row->ever_accepted += res->accepted > 0;
    row->invalid += (uint64_t)res->invalid;
    row->requests += res->requests;
    row->accepted += res->accepted;
    row->wrong += res->wrong;
    row->tail_accepted += res->tail_accepted;
    row->active += res->active;
    row->full += res->full;
    row->revocations += res->revocations;
    row->missing += res->missing;
    row->guesses += res->guesses;
    row->guesses_wrong += res->guesses_wrong;
    if (res->threshold >= 0.0) row->threshold[row->nthreshold++] = res->threshold;
    if (res->active_at_flip) {
        row->flip_wrong[row->active_at_flip] = res->flip_wrong;
        row->flip_requests[row->active_at_flip] = res->stopped ? res->flip_requests : SIM_NEVER;
        row->active_at_flip++;
        row->stopped += (uint64_t)res->stopped;
    }
}

/* Sort what the row's quantiles are read from. */
static void simRowClose(simRow *row) {
    qsort(row->first, row->runs, sizeof(uint64_t), simCmpU64);
    qsort(row->flip_wrong, row->active_at_flip, sizeof(uint64_t), simCmpU64);
    qsort(row->flip_requests, row->active_at_flip, sizeof(uint64_t), simCmpU64);
    qsort(row->threshold, row->nthreshold, sizeof(double), simCmpDouble);
}

static void simRowFree(simRow *row) {
    free(row->first);
    free(row->flip_wrong);
    free(row->flip_requests);
    free(row->threshold);
}

static void simScoreRow(const simSetup *su, uint64_t runs, uint64_t seed, uint64_t id,
                        simRow *row)
{
    uint64_t i;

    simRowInit(row, runs);
    for (i = 0; i < runs; i++) {
        simResult res;

        simScoreRun(su, simStream(seed, id * 1000003ull + i), &res);
        simRowAdd(row, &res);
    }
    simRowClose(row);
}

/* "0.452" or "none" for the median threshold of the runs that answer at
 * the end, when at least half of all runs do. */
static const char *simMedianThreshold(char *buf, size_t len, const simRow *row) {
    if (row->nthreshold * 2 < row->runs) snprintf(buf, len, "none");
    else snprintf(buf, len, "%.3f", row->threshold[row->nthreshold / 2]);
    return buf;
}

/* --------------------------------- (A) ---------------------------------- */

/* For comparison: the slow teacher against a gate that applies each answer
 * the moment it arrives. Returns 1 if some threshold of its 32 gets proven.
 * (gate_stats only: the real gate cannot be made to do this.) */
static int simNaiveRun(const simWorld *w, const simTruth *tr, double alpha, uint64_t seed) {
    double late[SIM_DELAY + 1], bar = gateBudgetLogThreshold(SIM_DELTA, 1, T);
    uint64_t due[SIM_DELAY + 1], rng = seed, i;
    uint32_t head = 0, tail = 0, t, n;
    gateRegion reg[T];
    gateBets bets;

    gateBetsInit(&bets, alpha);
    for (t = 0; t < T; t++) gateRegionInit(&reg[t]);
    for (i = 0; i < SIM_HORIZON; i++) {
        double score;
        int wrong;

        while (head != tail && due[head] <= i) {
            n = gateGridCount(tr->grid, T, late[head]);
            for (t = 0; t < n; t++) gateRegionUpdate(&reg[t], &bets, 1);
            head = (head + 1) % (SIM_DELAY + 1);
        }
        simDraw(w, &rng, &score, &wrong);
        if (wrong) {
            late[tail] = score;
            due[tail] = i + SIM_DELAY;
            tail = (tail + 1) % (SIM_DELAY + 1);
            continue;
        }
        n = gateGridCount(tr->grid, T, score);
        for (t = 0; t < n; t++) {
            gateRegionUpdate(&reg[t], &bets, 0);
            if (gateRegionCertifies(&reg[t], bar)) return 1;
        }
    }
    return 0;
}

static void simSafety(uint64_t runs, uint64_t seed) {
    static const double alphas[] = {0.05, 0.1};
    static const struct {
        int feedback;
        double loss;
        const char *name;
    } modes[] = {
        {SIM_PROMPT, 0.0, "prompt, in request order"},
        {SIM_ADAPTIVE, 0.0, "wrong answers 250 requests late"},
        {SIM_LOSSY, 0.02, "2% of the answers never sent"}
    };
    clock_t start = clock();
    uint64_t naive[2][2], i;
    size_t a, w, m;

    gate_test_current = "table A";
    printf("\n(A) SAFETY: SCORERS THAT DESERVE NO TRUST AT ANY THRESHOLD\n"
           "    delta = %.2f, audit = 0.05, %u requests per gate.\n"
           "    'flat': every score is wrong 1.25 alpha of the time. 'band': scores in [0.4, 0.7) are never\n"
           "    wrong, all others 2.5 alpha of the time, so that a clean band tempts the gate while every\n"
           "    region {score >= t} is still worse than alpha ('lowest risk' is the best of the 32). A gate\n"
           "    that EVER answers here has made the mistake delta pays for.\n\n",
           SIM_DELTA, SIM_HORIZON);
    printf("    %-5s  %-6s  %-11s  %-32s  %6s  %11s  %9s  %-16s  %5s\n", "alpha", "scorer",
           "lowest risk", "feedback", "gates", "ever ACTIVE", "frequency", "95% interval", "delta");
    for (a = 0; a < 2; a++) {
        simWorld worlds[2] = {
            {"flat", 0.0, 0.0, 1.0, SIM_FLAT, 0.0, 0.0},
            {"band", 0.0, 0.0, 1.0, SIM_BAND, 0.0, 0.0}
        };

        worlds[0].a = 1.25 * alphas[a];
        worlds[1].a = 2.5 * alphas[a];
        for (w = 0; w < 2; w++) {
            simTruth tr;

            simTruthOf(&worlds[w], alphas[a], &tr);
            CHECK(tr.best < 0 && tr.lowest_risk > 1.2 * alphas[a]);
            for (m = 0; m < 3; m++) {
                simSetup su = {alphas[a], 0.05, &worlds[w], &tr, NULL, modes[m].feedback,
                               modes[m].loss, 0, 0, 1000};
                simRow row;
                double lo, hi;

                simScoreRow(&su, runs, seed, 100 + a * 10 + w * 3 + m, &row);
                simWilson(row.ever_active, runs, &lo, &hi);
                printf("    %.2f   %-6s  %.4f       %-32s  %6llu  %11llu  %9.4f  [%.4f, %.4f]  %5.2f\n",
                       alphas[a], worlds[w].name, tr.lowest_risk, modes[m].name,
                       (unsigned long long)runs, (unsigned long long)row.ever_active,
                       (double)row.ever_active / (double)runs, lo, hi, SIM_DELTA);
                CHECK(lo <= SIM_DELTA);
                CHECK(row.invalid == row.ever_active); /* no threshold is a valid one */
                simRowFree(&row);
            }
            naive[a][w] = 0;
            for (i = 0; i < runs; i++)
                naive[a][w] += (uint64_t)simNaiveRun(&worlds[w], &tr, alphas[a],
                                                     simStream(seed, (150 + a * 2 + w) * 1000003ull + i));
            /* The slow teacher is a real threat to a gate without request order. */
            CHECK(naive[a][w] * 10 >= runs * 9);
        }
        printf("\n");
    }
    printf("    The slow teacher answers at once when the guess was right and 250 requests later when it was\n"
           "    wrong, so the first 250 answers a gate sees are all \"right\". Applying proof in request order\n"
           "    makes that harmless. For comparison, the same 32 betting tests fed in ARRIVAL order prove a\n"
           "    threshold against this teacher in %llu and %llu (alpha 0.05: flat, band), %llu and %llu (alpha\n"
           "    0.10) of %llu runs. A lost answer counts as a wrong guess when its window closes.\n"
           "    The table took %.1f s.\n",
           (unsigned long long)naive[0][0], (unsigned long long)naive[0][1],
           (unsigned long long)naive[1][0], (unsigned long long)naive[1][1],
           (unsigned long long)runs, simSeconds(start));
}

/* --------------------------------- (B) ---------------------------------- */

static const simWorld simUseful[] = {
    {"calibrated", 0.0, 0.0, 1.0, SIM_POWER, 1.0, 1.0},
    {"steep", 0.0, 0.0, 1.0, SIM_POWER, 0.5, 3.0},
    {"often 1.0", 0.4, 0.02, 1.0, SIM_POWER, 0.6, 1.0},
    {"below 0.9", 0.0, 0.0, 0.9, SIM_POWER, 0.5, 2.0},
    {"top 5% only", 0.0, 0.0, 1.0, SIM_STEP, 0.3, 0.95}
};
#define SIM_NUSEFUL (sizeof(simUseful) / sizeof(simUseful[0]))

static void simUsefulness(uint64_t runs, uint64_t seed) {
    static const double alphas[] = {0.1, 0.05};
    clock_t start = clock();
    char b1[32], b2[32], b3[32];
    size_t a, w;

    gate_test_current = "table B";
    printf("\n(B) USEFULNESS: SCORERS WHOSE RISK FALLS WITH THE SCORE\n"
           "    delta = %.2f, audit = 0.05, %u requests per gate, every answer prompt.\n"
           "        calibrated   a score s is wrong with probability 1 - s\n"
           "        steep        wrong with probability 0.5 (1 - s)^3\n"
           "        often 1.0    40%% of the scores are exactly 1.0 (2%% wrong), the others 0.6 (1 - s)\n"
           "        below 0.9    scores uniform on [0, 0.9), wrong with probability 0.5 (1 - s / 0.9)^2\n"
           "        top 5%% only  2%% wrong from 0.95 up, 30%% below\n"
           "    'best valid' is the lowest threshold of the grid whose risk is at most alpha and 'ideal' the\n"
           "    share a gate answering there would answer (spot checks deducted). 'the end' is the last\n"
           "    %u requests; the threshold is the median over the gates that answer then. 'wrong' is\n"
           "    pooled over all answers of all gates. 'bad threshold': gates that at any time answered at a\n"
           "    threshold whose true risk is above alpha, which delta = %.2f allows to that share of them.\n\n",
           SIM_DELTA, SIM_HORIZON, SIM_TAIL, SIM_DELTA);
    printf("    %-5s  %-11s  %-13s  %-26s  %-20s  %-15s  %-9s  %s\n", "alpha", "scorer", "first ACCEPT",
           "share of requests answered", "threshold at the end", "wrong among", "never", "bad threshold");
    printf("    %-5s  %-11s  %6s %6s  %8s %8s %8s  %9s %10s  %-15s  %-9s  %s\n", "", "", "median", "p90",
           "overall", "the end", "ideal", "median", "best valid", "the answered", "answered", "(gates)");
    for (a = 0; a < 2; a++) {
        for (w = 0; w < SIM_NUSEFUL; w++) {
            simSetup su = {alphas[a], 0.05, &simUseful[w], NULL, NULL, SIM_PROMPT, 0.0, 0, 0, 1000};
            simTruth tr;
            simRow row;
            double lo, hi;

            simTruthOf(&simUseful[w], alphas[a], &tr);
            CHECK(tr.best >= 0);
            su.truth = &tr;
            simScoreRow(&su, runs, seed, 200 + a * 10 + w, &row);
            simWilson(row.invalid, runs, &lo, &hi);
            printf("    %.2f   %-11s  %6s %6s  %7.1f%% %7.1f%% %7.1f%%  %9s %10.3f  %6.2f%% of %2.0f%%   %-9llu  %llu of %llu\n",
                   alphas[a], simUseful[w].name,
                   simFmt(b1, sizeof(b1), simQuantile(row.first, runs, 0.5)),
                   simFmt(b2, sizeof(b2), simQuantile(row.first, runs, 0.9)),
                   simPct(row.accepted, row.requests), simPct(row.tail_accepted, runs * SIM_TAIL),
                   100.0 * tr.mass[tr.best] * 0.95, simMedianThreshold(b3, sizeof(b3), &row),
                   tr.grid[tr.best], simPct(row.wrong, row.accepted), 100.0 * alphas[a],
                   (unsigned long long)(runs - row.ever_accepted),
                   (unsigned long long)row.invalid, (unsigned long long)runs);
            CHECK(lo <= SIM_DELTA);
            CHECK(row.accepted > 0 && (double)row.wrong <= alphas[a] * (double)row.accepted);
            simRowFree(&row);
        }
        printf("\n");
    }
    printf("    The table took %.1f s.\n", simSeconds(start));
}

/* --------------------------------- (C) ---------------------------------- */

static void simDrift(uint64_t runs, uint64_t seed) {
    static const simWorld broken = {"broken", 0.0, 0.0, 1.0, SIM_FLAT, 0.5, 0.0};
    static const struct {
        double audit, loss;
        int64_t tick_ms;
        const char *name;
    } rows[] = {
        {0.05, 0.0, 1000, "every answer prompt"},
        {0.20, 0.0, 1000, "every answer prompt"},
        {0.05, 0.01, 1000, "1% of the answers lost"},
        {0.20, 0.01, 1000, "1% of the answers lost"},
        {0.05, 0.01, 50, "1% lost, 20 requests/s"}
    };
    clock_t start = clock();
    char b[6][32];
    simTruth tr;
    size_t r;

    gate_test_current = "table C";
    simTruthOf(&simUseful[1], 0.1, &tr);
    printf("\n(C) DRIFT: A HEALTHY GATE WHOSE SCORER BREAKS\n"
           "    Scorer 'steep', alpha = 0.10, delta = %.2f. From request %u on, every score is wrong half\n"
           "    of the time. Counted from there until the drift alarm stops the gate: the wrong answers it\n"
           "    gave, and the requests that went by. Gates that were not answering at the break are left out.\n\n",
           SIM_DELTA, SIM_FLIP);
    printf("    %-5s  %-24s  %5s  %7s  %-24s  %-24s\n", "audit", "feedback", "gates", "stopped",
           "wrong answers given", "requests until the stop");
    printf("    %-5s  %-24s  %5s  %7s  %7s %7s %7s   %7s %7s %7s\n", "", "", "", "", "median", "p90",
           "max", "median", "p90", "max");
    for (r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        simSetup su = {0.1, rows[r].audit, &simUseful[1], &tr, &broken,
                       rows[r].loss > 0.0 ? SIM_LOSSY : SIM_PROMPT, rows[r].loss, 0, 0,
                       rows[r].tick_ms};
        simRow row;
        uint64_t n;

        simScoreRow(&su, runs, seed, 300 + r, &row);
        n = row.active_at_flip;
        CHECK(n * 2 > runs && row.stopped == n);
        if (n) {
            printf("    %.2f   %-24s  %5llu  %7llu  %7s %7s %7s   %7s %7s %7s\n", rows[r].audit,
                   rows[r].name, (unsigned long long)n, (unsigned long long)row.stopped,
                   simFmt(b[0], 32, simQuantile(row.flip_wrong, n, 0.5)),
                   simFmt(b[1], 32, simQuantile(row.flip_wrong, n, 0.9)),
                   simFmt(b[2], 32, row.flip_wrong[n - 1]),
                   simFmt(b[3], 32, simQuantile(row.flip_requests, n, 0.5)),
                   simFmt(b[4], 32, simQuantile(row.flip_requests, n, 0.9)),
                   simFmt(b[5], 32, row.flip_requests[n - 1]));
        }
        simRowFree(&row);
    }
    printf("\n    Spot checks are applied in request order like all proof, so one that never gets its answer\n"
           "    holds the following ones back until its feedback window closes (300 requests at one request\n"
           "    per second, 6000 at twenty), and the alarm waits with them.\n"
           "    The table took %.1f s.\n", simSeconds(start));
}

/* --------------------------------- (D) ---------------------------------- */

static void simLostFeedback(uint64_t runs, uint64_t seed) {
    static const struct {
        int64_t tick_ms;
        double loss;
        uint64_t outage;
        const char *traffic, *lost;
    } rows[] = {
        {1000, 0.0, 0, "1 request/s", "nothing"},
        {1000, 0.001, 0, "1 request/s", "0.1%"},
        {1000, 0.01, 0, "1 request/s", "1%"},
        {1000, 0.05, 0, "1 request/s", "5%"},
        {1000, 0.0, 600, "1 request/s", "all for 10 minutes"},
        {50, 0.0, 0, "20 requests/s", "nothing"},
        {50, 0.001, 0, "20 requests/s", "0.1%"},
        {50, 0.01, 0, "20 requests/s", "1%"},
        {50, 0.05, 0, "20 requests/s", "5%"}
    };
    clock_t start = clock();
    char b1[32], b2[32], b3[32];
    simTruth tr;
    size_t r;

    gate_test_current = "table D";
    simTruthOf(&simUseful[1], 0.1, &tr);
    printf("\n(D) LOST FEEDBACK: REQUESTS THAT NEVER GET THEIR GLEARN\n"
           "    Scorer 'steep', alpha = 0.10, delta = %.2f, audit = 0.05, %u requests per gate. Of the\n"
           "    answers the gate asked for, the given share is never sent; the last row of the first block\n"
           "    loses every answer to the requests %u..%u instead. The feedback window is five minutes:\n"
           "    300 requests at one request per second, 6000 at twenty.\n\n",
           SIM_DELTA, SIM_HORIZON, SIM_FLIP, SIM_FLIP + 599);
    printf("    %-13s  %-18s  %-8s  %-13s  %-18s  %-9s  %-8s  %-7s  %-8s  %s\n", "traffic", "answers lost",
           "time", "first ACCEPT", "share of requests", "threshold", "wrong", "drift", "counted",
           "refused:");
    printf("    %-13s  %-18s  %-8s  %6s %6s  %8s %9s  %-9s  %-8s  %-7s  %-8s  %s\n", "", "", "ACTIVE",
           "median", "p90", "answered", "the end", "the end", "answered", "alarms", "as wrong",
           "gate full");
    for (r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        simSetup su = {0.1, 0.05, &simUseful[1], &tr, NULL,
                       rows[r].loss > 0.0 ? SIM_LOSSY : SIM_PROMPT, rows[r].loss, SIM_FLIP,
                       rows[r].outage, rows[r].tick_ms};
        simRow row;

        if (r == 5) printf("\n");
        simScoreRow(&su, runs, seed, 400 + r, &row);
        printf("    %-13s  %-18s  %6.1f%%   %6s %6s  %7.1f%% %8.1f%%  %-9s  %6.2f%%   %7.2f  %8.1f  %7.1f%%\n",
               rows[r].traffic, rows[r].lost, simPct(row.active, row.requests),
               simFmt(b1, sizeof(b1), simQuantile(row.first, runs, 0.5)),
               simFmt(b2, sizeof(b2), simQuantile(row.first, runs, 0.9)),
               simPct(row.accepted, row.requests), simPct(row.tail_accepted, runs * SIM_TAIL),
               simMedianThreshold(b3, sizeof(b3), &row), simPct(row.wrong, row.accepted),
               (double)row.revocations / (double)runs, (double)row.missing / (double)runs,
               simPct(row.full, row.requests));
        CHECK((double)row.wrong <= 0.1 * (double)row.accepted);
        simRowFree(&row);
    }
    printf("\n    'time ACTIVE' is the share of requests asked while a generation was answering. 'drift alarms'\n"
           "    and 'counted as wrong' are per gate: a lost answer is a wrong guess to the proof and, when it\n"
           "    was a spot check of an answer, to the drift detector too. While a lost answer waits for its\n"
           "    window to close the proof behind it waits as well; a gate that is still calibrating records\n"
           "    every request, fills up (4096 waiting) and refuses the rest.\n"
           "    The table took %.1f s.\n", simSeconds(start));
}

/* --------------------------------- (E) ---------------------------------- */

static void simClassifier(uint64_t runs, uint64_t seed) {
    static const char *const names[] = {"pure noise", "learnable"};
    clock_t start = clock();
    char b1[32], b2[32];
    int learnable;

    gate_test_current = "table E";
    printf("\n(E) CLASSIFIER GATE: TWO LABELS, FOUR WORDS PER REQUEST\n"
           "    alpha = 0.10, delta = %.2f, audit = 0.05, a snapshot every 1000 examples, %u requests per\n"
           "    gate, every answer prompt. 'learnable': the label is the topic the words were drawn for\n"
           "    (about 11%% of the requests are ambiguous even to a perfect reader). 'pure noise': the label\n"
           "    is a coin flip that has nothing to do with the words.\n\n", SIM_DELTA, SIM_HORIZON);
    printf("    %-10s  %5s  %-34s  %-13s  %-18s  %-11s  %s\n", "problem", "gates", "gates that ever answered",
           "first ACCEPT", "share of requests", "snapshots", "wrong among");
    printf("    %-10s  %5s  %6s %9s  %-16s  %6s %6s  %8s %9s  %-11s  %s\n", "", "", "", "frequency",
           "95% interval", "median", "p90", "answered", "the end", "guess wrong", "the answered");
    for (learnable = 0; learnable < 2; learnable++) {
        simRow row;
        double lo, hi;
        uint64_t i;

        simRowInit(&row, runs);
        for (i = 0; i < runs; i++) {
            simResult res;

            simClassRun(learnable, 0.1, simStream(seed, (500 + (uint64_t)learnable) * 1000003ull + i),
                        &res);
            simRowAdd(&row, &res);
        }
        simRowClose(&row);
        simWilson(row.ever_active, runs, &lo, &hi);
        printf("    %-10s  %5llu  %6llu %9.4f  [%.4f, %.4f]  %6s %6s  %7.1f%% %8.1f%%  %10.1f%%  ",
               names[learnable], (unsigned long long)runs, (unsigned long long)row.ever_active,
               (double)row.ever_active / (double)runs, lo, hi,
               simFmt(b1, sizeof(b1), simQuantile(row.first, runs, 0.5)),
               simFmt(b2, sizeof(b2), simQuantile(row.first, runs, 0.9)),
               simPct(row.accepted, row.requests), simPct(row.tail_accepted, runs * SIM_TAIL),
               simPct(row.guesses_wrong, row.guesses));
        if (row.accepted) printf("%.2f%% of 10%%\n", simPct(row.wrong, row.accepted));
        else printf("-\n");
        if (learnable) {
            CHECK(row.ever_active * 10 >= runs * 9);
            CHECK((double)row.wrong <= 0.1 * (double)row.accepted);
        } else {
            CHECK(lo <= SIM_DELTA); /* a gate trained on noise must not answer */
        }
        simRowFree(&row);
    }
    printf("\n    'snapshots guess wrong': how often the frozen model in charge disagreed with the teacher,\n"
           "    over all requests, answered or not.\n"
           "    The table took %.1f s.\n", simSeconds(start));
}

/* ---------------------------------- main --------------------------------- */

int main(int argc, char **argv) {
    uint64_t runs = 400, seed = 20261128;
    clock_t start = clock();

    if (argc > 1) runs = strtoull(argv[1], NULL, 10);
    if (argc > 2) seed = strtoull(argv[2], NULL, 10);
    if (argc > 3 || runs < 1 || runs > 10000000) {
        fprintf(stderr, "usage: %s [runs (1..10000000) [seed]]\n", argv[0]);
        return 2;
    }
    printf("GATE protocol Monte Carlo: runs = %llu, seed = %llu\n"
           "Every row simulates that many gates, each with its own client and teacher, through gate_core.c.\n",
           (unsigned long long)runs, (unsigned long long)seed);
    simSafety(runs, seed);
    simUsefulness(runs, seed);
    simDrift(runs, seed);
    simLostFeedback(runs, seed);
    simClassifier(runs, seed);
    printf("\nTotal CPU time %.1f s. Consistency checks: ", simSeconds(start));
    return TEST_REPORT();
}
