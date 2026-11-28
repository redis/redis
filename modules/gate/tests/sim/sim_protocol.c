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
 * 'runs' (default 400, about a minute and a quarter of CPU) is the number
 * of gates simulated for every row of every table. Everything is
 * deterministic for a given (runs, seed).
 *
 * Where sim_certify studies the betting test alone, this program drives
 * real gates through gateMaintain() / gateEvaluateFor() / gatePredict() /
 * gateLearn() the way the module does for a client and its teacher, and
 * prints:
 *
 *   (A) safety: how often a gate ever answers at a threshold that does not
 *       deserve it, when the teacher answers at once and when it delays or
 *       withholds exactly the answers that say "wrong" (must stay below
 *       delta);
 *   (B) usefulness: how soon and how much a gate answers for scorers whose
 *       risk falls with the score, and how wrong its answers are;
 *   (C) drift: wrong answers given between the moment a scorer breaks and
 *       the moment the gate stops, with a prompt teacher, a slow one, one
 *       that loses answers, and one that holds back every "wrong";
 *   (D) what lost answers cost, at three request rates;
 *   (E) what a slow teacher costs: answers in order, out of order, and
 *       with a heavy tail;
 *   (F) a feedback outage: how long the gate goes on answering, that it
 *       stops, that it comes back, and what it costs in snapshots;
 *   (G) a classifier gate on a problem it can learn, on pure noise, and
 *       through an outage.
 *
 * A SCORER is a distribution of scores and, for each score, the probability
 * that the cheap result disagrees with the teacher. Scores are uniform on
 * [0, top) plus, for some scorers, a share that is exactly 1.0. The risk of
 * every threshold of the grid is computed from that definition by numerical
 * integration, never from simulated requests.
 *
 * A TEACHER decides when the answer to a request arrives, if it does. The
 * client passes every answer on with GLEARN the moment it has it.
 *
 * TIME. The feedback timeout is the default, one minute: proof waits 15
 * seconds for a late answer, then moves on and counts the request as a
 * wrong guess until its answer arrives; after a minute the id is forgotten.
 * How many requests that is depends on the request rate, which is why the
 * tables say it: 15 and 60 requests at one request per second, 300 and
 * 1,200 at twenty.
 *
 * Simulation is evidence, not proof, and all requests here are independent
 * and alike, which is what the guarantee itself assumes. The exit status is
 * non zero if a consistency check fails: gates that answered at a threshold
 * worse than alpha significantly more often than delta allows, an error
 * rate among the answers above alpha, a broken scorer that was not stopped
 * although the teacher said so, a gate that went on answering through a
 * long outage or used up snapshot numbers during one.
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
#define SIM_FLIP 10000u    /* the request at which a scorer breaks or an outage begins */
#define SIM_TIMEOUT GATE_DEFAULT_FEEDBACK_TIMEOUT_MS
#define SIM_GRACE (SIM_TIMEOUT / GATE_GRACE_DIVISOR)

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

/* -------------------------------- teachers -------------------------------
 * When the answer to a request arrives. */

enum { SIM_PROMPT, SIM_FIXED, SIM_UNIFORM, SIM_PARETO, SIM_WRONG_LATE, SIM_WRONG_NEVER };

typedef struct simTeacher {
    int kind;       /* PROMPT: at once.
                     * FIXED: 'lo' ms later, so in request order.
                     * UNIFORM: between 'lo' and 'hi' ms later.
                     * PARETO: at least 'lo' ms later, with a heavy tail:
                     *   P(later than x) = (lo / x)^1.5.
                     * WRONG_LATE: "right" at once, "wrong" 'lo' ms later.
                     * WRONG_NEVER: "right" at once, "wrong" never. */
    int64_t lo, hi;
    double loss;    /* on top of that, the share of answers never sent */
} simTeacher;

typedef struct simAnswer {
    int64_t due;
    uint64_t seq;
    int32_t result;
} simAnswer;

/* The answers on their way: a binary heap, the next one to arrive on top. */
typedef struct simHeap {
    simAnswer *a;
    uint32_t n, cap;
} simHeap;

static int simSooner(const simAnswer *x, const simAnswer *y) {
    return x->due < y->due || (x->due == y->due && x->seq < y->seq);
}

static void simHeapPush(simHeap *h, simAnswer x) {
    uint32_t i;

    if (h->n == h->cap) {
        h->cap = h->cap ? 2 * h->cap : 256;
        h->a = realloc(h->a, sizeof(simAnswer) * h->cap);
        if (h->a == NULL) simDie("out of memory");
    }
    for (i = h->n++; i > 0 && simSooner(&x, &h->a[(i - 1) / 2]); i = (i - 1) / 2)
        h->a[i] = h->a[(i - 1) / 2];
    h->a[i] = x;
}

static simAnswer simHeapPop(simHeap *h) {
    simAnswer top = h->a[0], last = h->a[--h->n];
    uint32_t i = 0, c;

    while ((c = 2 * i + 1) < h->n) {
        if (c + 1 < h->n && simSooner(&h->a[c + 1], &h->a[c])) c++;
        if (!simSooner(&h->a[c], &last)) break;
        h->a[i] = h->a[c];
        i = c;
    }
    h->a[i] = last;
    return top;
}

/* -------------------------------- one gate ------------------------------- */

typedef struct simSetup {
    double alpha, audit;
    const simWorld *world;
    const simTruth *truth;  /* what 'world' does at each threshold */
    const simWorld *broken; /* takes over at request SIM_FLIP; NULL: never */
    simTeacher teacher;
    uint64_t outage;        /* requests from SIM_FLIP on during which no
                             * answer arrives; 0: no outage */
    int64_t tick_ms;        /* time between two requests */
    uint64_t horizon;       /* requests per run */
} simSetup;

typedef struct simResult {
    uint64_t requests, accepted, wrong; /* wrong: among the accepted */
    uint64_t tail_accepted;             /* accepted among the last SIM_TAIL */
    uint64_t first_accept;              /* requests until the first ACCEPT */
    uint64_t active;                    /* requests asked of an ACTIVE gate */
    uint64_t full, promotions, revocations, missing, generations;
    uint64_t late;                      /* answers that arrived when proof had moved on */
    uint64_t refused;                   /* ... when the id was forgotten */
    uint64_t guesses, guesses_wrong;    /* (G): what the snapshots guessed */
    double threshold;                   /* at the end; negative: none */
    int invalid;                        /* answered at a risk above alpha */
    int active_at_flip, stopped;        /* (C), (F): answering when it began; stopped */
    uint64_t flip_wrong, flip_requests; /* (C): from the flip to the stop */
    /* (F), (G): an outage. */
    uint64_t out_stop;                  /* requests into it when the gate stopped */
    uint64_t out_accepted;              /* answers given during it */
    uint64_t out_back;                  /* requests after it until the next ACCEPT */
    uint64_t out_generations;           /* snapshot numbers used during it */
    uint64_t end_generations;           /* ... from its beginning to the end of the run */
    int out_blind;                      /* the gate saw that it was blind */
    int model_kept;                     /* (G): the model after it had learned no less */
} simResult;

static void simFinish(gate *g, simResult *res) {
    res->full = g->stats.full;
    res->promotions = g->stats.promotions;
    res->revocations = g->stats.revocations;
    res->missing = g->stats.missing;
    res->generations = g->generations;
    res->threshold = gateRowThreshold(g, 0);
    gateFree(g);
}

/* The threshold a score gate answers at must be one whose risk is at most
 * alpha. Looked at after every request and every answer, so every policy
 * the gate ever had is seen. */
static void simWatch(const gate *g, const simSetup *su, int healthy, simResult *res) {
    if (healthy && g->serving && g->serving->policy[0] >= 0 &&
        !(su->truth->risk[g->serving->policy[0]] <= su->alpha))
        res->invalid = 1;
}

/* GLEARN. */
static void simLearn(gate *g, int64_t now, uint64_t seq, int32_t result, simResult *res) {
    const gateObs *o = gateFindObservation(g, seq);
    int moved_on = o != NULL && (o->flags & GATE_OBS_PROVISIONAL) != 0;

    if (!gateLearn(g, now, g->epoch, seq, result)) res->refused++;
    else if (moved_on) res->late++;
}

/* When the answer to a request asked at 'now' arrives; -1: never. An
 * answer that takes the whole feedback timeout or longer would be refused:
 * it counts as refused here and is not sent. */
static int64_t simArrival(const simTeacher *te, uint64_t *rng, int64_t now, int wrong,
                          simResult *res)
{
    double late = 0.0;

    if (te->loss > 0.0 && gate_test_uniform(rng) < te->loss) return -1;
    switch (te->kind) {
    case SIM_FIXED: late = (double)te->lo; break;
    case SIM_UNIFORM:
        late = (double)te->lo + gate_test_uniform(rng) * (double)(te->hi - te->lo);
        break;
    case SIM_PARETO:
        late = (double)te->lo * pow(1.0 - gate_test_uniform(rng), -1.0 / 1.5);
        break;
    case SIM_WRONG_LATE: late = wrong ? (double)te->lo : 0.0; break;
    case SIM_WRONG_NEVER:
        if (wrong) return -1;
        break;
    default: break;
    }
    if (late >= (double)SIM_TIMEOUT) {
        res->refused++;
        return -1;
    }
    return now + (int64_t)late;
}

/* One score gate, one client, one teacher. */
static void simScoreRun(const simSetup *su, uint64_t seed, simResult *res) {
    const uint64_t out_from = SIM_FLIP, out_to = SIM_FLIP + su->outage;
    const int64_t down_from = (int64_t)out_from * su->tick_ms, down_to = (int64_t)out_to * su->tick_ms;
    simHeap heap = {NULL, 0, 0};
    uint64_t rng = seed, revoked_before = 0, gens_before = 0, i;
    gateConfig cfg;
    char err[128];
    gate *g;

    memset(res, 0, sizeof(*res));
    res->first_accept = res->out_stop = res->out_back = SIM_NEVER;
    res->model_kept = 1;
    gateConfigInit(&cfg);
    cfg.alpha = su->alpha;
    cfg.delta = SIM_DELTA;
    cfg.audit = su->audit;
    if (gateConfigValidate(&cfg, err, sizeof(err)) != GATE_OK) simDie(err);
    g = gateCreate(&cfg, gate_test_rand64(&rng), gate_test_rand64(&rng), 0);
    if (g == NULL) simDie("out of memory");
    gateConfigFree(&cfg);

    for (i = 0; i < su->horizon; i++) {
        const simWorld *w = su->broken && i >= SIM_FLIP ? su->broken : su->world;
        int64_t now = (int64_t)i * su->tick_ms, due;
        int in_outage = su->outage && i >= out_from && i < out_to;
        gatePredictResult r;
        double score, u;
        gateEval ev;
        int wrong;

        /* The answers that have arrived since the last request. */
        while (heap.n && heap.a[0].due <= now) {
            simAnswer a = simHeapPop(&heap);

            simLearn(g, a.due, a.seq, a.result, res);
            simWatch(g, su, w == su->world, res);
        }
        gateMaintain(g, now);
        if ((su->broken || su->outage) && i == SIM_FLIP) {
            res->active_at_flip = g->serving != NULL;
            revoked_before = g->stats.revocations;
            gens_before = g->generations;
        }
        if (su->outage && i == out_to) res->out_generations = g->generations - gens_before;
        simDraw(w, &rng, &score, &wrong);
        u = gate_test_uniform(&rng);
        if (gateEvaluateFor(g, NULL, score, u, &ev) != GATE_OK ||
            gatePredict(g, now, u, &ev, NULL, &r) != GATE_OK)
            simDie("gatePredict failed");
        res->requests++;
        if (g->serving) res->active++;

        if (r.action == GATE_ACTION_ACCEPT) {
            res->accepted++;
            res->wrong += (uint64_t)wrong;
            if (res->first_accept == SIM_NEVER) res->first_accept = i + 1;
            if (i + SIM_TAIL >= su->horizon) res->tail_accepted++;
            if (su->broken && i >= SIM_FLIP) res->flip_wrong += (uint64_t)wrong;
            if (in_outage) res->out_accepted++;
            if (su->outage && i >= out_to && res->out_back == SIM_NEVER) res->out_back = i - out_to + 1;
        } else if (r.seq != 0) {
            due = simArrival(&su->teacher, &rng, now, wrong, res);
            /* During an outage nothing arrives, whenever it was asked. */
            if (su->outage && due >= down_from && due < down_to) due = -1;
            if (due == now) {
                simLearn(g, now, r.seq, !wrong, res);
            } else if (due > now) {
                simAnswer a;

                a.due = due;
                a.seq = r.seq;
                a.result = !wrong;
                simHeapPush(&heap, a);
            }
        }
        simWatch(g, su, w == su->world, res);
        if (g->blind && in_outage) res->out_blind = 1;
        if (in_outage && res->out_stop == SIM_NEVER && g->serving == NULL) res->out_stop = i - out_from + 1;
        if (su->broken && i >= SIM_FLIP && g->stats.revocations > revoked_before) {
            res->stopped = 1;
            res->flip_requests = i - SIM_FLIP + 1;
            break;
        }
    }
    if (su->outage) {
        res->stopped = g->stats.revocations > revoked_before;
        res->end_generations = g->generations - gens_before;
    }
    free(heap.a);
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

/* One classifier gate for SIM_HORIZON requests, one per second, every answer
 * at once; 'outage': requests from SIM_FLIP on whose answers never come. */
static void simClassRun(int learnable, double alpha, uint64_t outage, uint64_t seed,
                        simResult *res)
{
    static const char *const labels[] = {"no", "yes"};
    static const size_t lens[] = {2, 3};
    const uint64_t out_to = SIM_FLIP + outage;
    uint64_t rng = seed, updates_before = 0, gens_before = 0, revoked_before = 0, i;
    gateConfig cfg;
    char err[128];
    gate *g;

    memset(res, 0, sizeof(*res));
    res->first_accept = res->out_stop = res->out_back = SIM_NEVER;
    res->model_kept = 1;
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
        int in_outage = outage && i >= SIM_FLIP && i < out_to;
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
        if (outage && i == SIM_FLIP) {
            res->active_at_flip = g->serving != NULL;
            updates_before = g->trainer.updates;
            gens_before = g->generations;
            revoked_before = g->stats.revocations;
        }
        if (outage && i == out_to) {
            res->out_generations = g->generations - gens_before;
            res->model_kept = g->trainer.updates >= updates_before;
        }
        if (gateEvaluateFor(g, &x, 0.0, u, &ev) != GATE_OK) simDie("gateEvaluateFor failed");
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
            if (in_outage) res->out_accepted++;
            if (outage && i >= out_to && res->out_back == SIM_NEVER) res->out_back = i - out_to + 1;
        } else if (r.seq != 0 && !in_outage) {
            if (!gateLearn(g, now, g->epoch, r.seq, label)) res->refused++;
        }
        if (g->blind && in_outage) res->out_blind = 1;
        if (in_outage && res->out_stop == SIM_NEVER && g->serving == NULL) res->out_stop = i - SIM_FLIP + 1;
    }
    if (outage) {
        res->stopped = g->stats.revocations > revoked_before;
        res->end_generations = g->generations - gens_before;
    }
    simFinish(g, res);
}

/* ------------------------------ one table row ---------------------------- */

typedef struct simRow {
    uint64_t runs, ever_active, ever_accepted, invalid;
    uint64_t requests, accepted, wrong, tail_accepted, active;
    uint64_t full, revocations, missing, late, refused, guesses, guesses_wrong;
    uint64_t active_at_flip, stopped;
    uint64_t out_stopped, out_blind, out_generations, end_generations, model_lost;
    uint64_t *first;      /* per run */
    uint64_t *flip_wrong; /* per run that was answering at SIM_FLIP: */
    uint64_t *flip_requests;
    uint64_t *out_stop, *out_accepted, *out_back;
    double *threshold;    /* per run that answers at the end */
    uint64_t nthreshold;
} simRow;

static void simRowInit(simRow *row, uint64_t runs) {
    memset(row, 0, sizeof(*row));
    row->first = simAlloc(runs, sizeof(uint64_t));
    row->flip_wrong = simAlloc(runs, sizeof(uint64_t));
    row->flip_requests = simAlloc(runs, sizeof(uint64_t));
    row->out_stop = simAlloc(runs, sizeof(uint64_t));
    row->out_accepted = simAlloc(runs, sizeof(uint64_t));
    row->out_back = simAlloc(runs, sizeof(uint64_t));
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
    row->late += res->late;
    row->refused += res->refused;
    row->guesses += res->guesses;
    row->guesses_wrong += res->guesses_wrong;
    if (res->threshold >= 0.0) row->threshold[row->nthreshold++] = res->threshold;
    if (res->active_at_flip) {
        uint64_t k = row->active_at_flip++;

        row->flip_wrong[k] = res->flip_wrong;
        row->flip_requests[k] = res->stopped ? res->flip_requests : SIM_NEVER;
        row->out_stop[k] = res->out_stop;
        row->out_accepted[k] = res->out_accepted;
        row->out_back[k] = res->out_back;
        row->stopped += (uint64_t)res->stopped;
        row->out_stopped += res->out_stop != SIM_NEVER;
        row->out_blind += (uint64_t)res->out_blind;
    }
    if (res->out_generations > row->out_generations) row->out_generations = res->out_generations;
    row->end_generations += res->end_generations;
    row->model_lost += !res->model_kept;
}

/* Sort what the row's quantiles are read from. */
static void simRowClose(simRow *row) {
    qsort(row->first, row->runs, sizeof(uint64_t), simCmpU64);
    qsort(row->flip_wrong, row->active_at_flip, sizeof(uint64_t), simCmpU64);
    qsort(row->flip_requests, row->active_at_flip, sizeof(uint64_t), simCmpU64);
    qsort(row->out_stop, row->active_at_flip, sizeof(uint64_t), simCmpU64);
    qsort(row->out_accepted, row->active_at_flip, sizeof(uint64_t), simCmpU64);
    qsort(row->out_back, row->active_at_flip, sizeof(uint64_t), simCmpU64);
    qsort(row->threshold, row->nthreshold, sizeof(double), simCmpDouble);
}

static void simRowFree(simRow *row) {
    free(row->first);
    free(row->flip_wrong);
    free(row->flip_requests);
    free(row->out_stop);
    free(row->out_accepted);
    free(row->out_back);
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

static double simPer(uint64_t total, uint64_t runs) {
    return (double)total / (double)runs;
}

static const simTeacher simAtOnce = {SIM_PROMPT, 0, 0, 0.0};

/* --------------------------------- (A) ---------------------------------- */

#define SIM_A_HORIZON 10000u /* requests per run on the scorers that deserve no trust */
#define SIM_A_TICK 50        /* twenty requests per second */
#define SIM_A_LATE 45000     /* how late the slow teacher sends "wrong", ms */

static const struct {
    simTeacher teacher;
    const char *name;
} simAdversaries[] = {
    {{SIM_PROMPT, 0, 0, 0.0}, "every answer at once"},
    {{SIM_WRONG_LATE, SIM_A_LATE, 0, 0.0}, "\"wrong\" 45 s late, \"right\" at once"},
    {{SIM_WRONG_NEVER, 0, 0, 0.0}, "\"wrong\" never, \"right\" at once"},
    {{SIM_UNIFORM, 16000, 59000, 0.0}, "every answer 16 to 59 s late"}
};
#define SIM_NADVERSARIES (sizeof(simAdversaries) / sizeof(simAdversaries[0]))

/* For comparison: the slow teacher against a gate that applies each answer
 * the moment it arrives. Returns 1 if some threshold of its 32 gets proven.
 * (gate_stats only: the real gate cannot be made to do this.) */
static int simNaiveRun(const simWorld *w, const simTruth *tr, double alpha, uint64_t seed) {
    enum { DELAY = SIM_A_LATE / SIM_A_TICK };
    static double late[DELAY + 1];
    static uint64_t due[DELAY + 1];
    double bar = gateBudgetLogThreshold(SIM_DELTA, 1, T);
    uint64_t rng = seed, i;
    uint32_t head = 0, tail = 0, t, n;
    gateRegion reg[T];
    gateBets bets;

    gateBetsInit(&bets, alpha);
    for (t = 0; t < T; t++) gateRegionInit(&reg[t]);
    for (i = 0; i < SIM_A_HORIZON; i++) {
        double score;
        int wrong;

        while (head != tail && due[head] <= i) {
            n = gateGridCount(tr->grid, T, late[head]);
            for (t = 0; t < n; t++) gateRegionUpdate(&reg[t], &bets, 1);
            head = (head + 1) % (DELAY + 1);
        }
        simDraw(w, &rng, &score, &wrong);
        if (wrong) {
            late[tail] = score;
            due[tail] = i + DELAY;
            tail = (tail + 1) % (DELAY + 1);
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
    static const simWorld useful = {"calibrated", 0.0, 0.0, 1.0, SIM_POWER, 1.0, 1.0};
    clock_t start = clock();
    uint64_t naive[2][2], pooled = 0, pooled_runs = 0, i;
    double lo, hi;
    simTruth tr;
    size_t a, w, m;

    gate_test_current = "table A";
    printf("\n(A) SAFETY: A TEACHER WHO DELAYS OR WITHHOLDS EXACTLY THE ANSWERS THAT SAY \"WRONG\"\n"
           "    delta = %.2f, audit = 0.05, twenty requests per second (proof waits 300 requests for a late\n"
           "    answer, the id of a request is forgotten after 1200).\n\n"
           "    First, scorers that deserve no trust at any threshold, %u requests per gate. 'flat': every\n"
           "    score is wrong 1.25 alpha of the time. 'band': scores in [0.4, 0.7) are never wrong, all others\n"
           "    2.5 alpha of the time, so that a clean band tempts the gate while every region {score >= t} is\n"
           "    still worse than alpha ('lowest risk' is the best of the 32). A gate that EVER answers here\n"
           "    has made the mistake delta pays for. 'late': answers per gate that arrived after proof had\n"
           "    moved on without them (their requests were counted as wrong guesses meanwhile) and were used.\n\n",
           SIM_DELTA, SIM_A_HORIZON);
    printf("    %-5s  %-6s  %-11s  %-36s  %6s  %11s  %9s  %-16s  %5s  %9s\n", "alpha", "scorer",
           "lowest risk", "teacher", "gates", "ever ACTIVE", "frequency", "95% interval", "delta",
           "late");
    for (a = 0; a < 2; a++) {
        simWorld worlds[2] = {
            {"flat", 0.0, 0.0, 1.0, SIM_FLAT, 0.0, 0.0},
            {"band", 0.0, 0.0, 1.0, SIM_BAND, 0.0, 0.0}
        };

        worlds[0].a = 1.25 * alphas[a];
        worlds[1].a = 2.5 * alphas[a];
        for (w = 0; w < 2; w++) {
            simTruthOf(&worlds[w], alphas[a], &tr);
            CHECK(tr.best < 0 && tr.lowest_risk > 1.2 * alphas[a]);
            for (m = 0; m < SIM_NADVERSARIES; m++) {
                simSetup su = {alphas[a], 0.05, &worlds[w], &tr, NULL, simAdversaries[m].teacher,
                               0, SIM_A_TICK, SIM_A_HORIZON};
                simRow row;

                simScoreRow(&su, runs, seed, 100 + a * 20 + w * 5 + m, &row);
                simWilson(row.ever_active, runs, &lo, &hi);
                printf("    %.2f   %-6s  %.4f       %-36s  %6llu  %11llu  %9.4f  [%.4f, %.4f]  %5.2f  %9.0f\n",
                       alphas[a], worlds[w].name, tr.lowest_risk, simAdversaries[m].name,
                       (unsigned long long)runs, (unsigned long long)row.ever_active,
                       (double)row.ever_active / (double)runs, lo, hi, SIM_DELTA,
                       simPer(row.late, runs));
                CHECK(lo <= SIM_DELTA);
                CHECK(row.invalid == row.ever_active); /* no threshold is a valid one */
                /* The slow teachers really are slow: proof moved on. */
                CHECK(m == 0 || row.late + row.missing > runs);
                pooled += row.ever_active;
                pooled_runs += runs;
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
    simWilson(pooled, pooled_runs, &lo, &hi);
    printf("    All %llu gates together: %llu ever answered, %.5f, 95%% interval [%.5f, %.5f].\n\n"
           "    The slow teacher answers at once when the guess was right and 45 seconds (900 requests) later\n"
           "    when it was wrong, so the first 900 answers a gate sees are all \"right\". Proof is counted in\n"
           "    request order: a request without its answer holds the later ones up, and when proof moves on\n"
           "    after 15 seconds the request is counted as a wrong guess, which is what its answer will say.\n"
           "    For comparison, the same 32 betting tests fed in ARRIVAL order prove a threshold against this\n"
           "    teacher in %llu and %llu (alpha 0.05: flat, band), %llu and %llu (alpha 0.10) of %llu runs.\n"
           "    When every answer is late, most requests are counted as wrong guesses first and set right\n"
           "    when their answer arrives: the last teacher is there for that path.\n\n",
           (unsigned long long)pooled_runs, (unsigned long long)pooled, (double)pooled / (double)pooled_runs,
           lo, hi, (unsigned long long)naive[0][0], (unsigned long long)naive[0][1],
           (unsigned long long)naive[1][0], (unsigned long long)naive[1][1],
           (unsigned long long)runs);

    /* The same teachers and a scorer with thresholds of both kinds. */
    simTruthOf(&useful, 0.1, &tr);
    CHECK(tr.best > 0);
    printf("    Second, a scorer that deserves trust at some thresholds only: 'calibrated' (a score s is wrong\n"
           "    with probability 1 - s), alpha = 0.10, %u requests per gate. The thresholds from %.3f up are\n"
           "    good; the %d below it are not, the nearest by a hair (its risk is %.4f). 'bad threshold': gates\n"
           "    that at any time answered at one of those. 'stops': times per gate that a gate which was\n"
           "    answering stopped.\n\n", SIM_HORIZON, tr.grid[tr.best], tr.best, tr.risk[tr.best - 1]);
    printf("    %-36s  %6s  %13s  %9s  %-16s  %5s  %8s  %-13s  %6s  %6s\n", "teacher", "gates",
           "bad threshold", "frequency", "95% interval", "delta", "answered", "wrong among", "late",
           "stops");
    for (m = 0; m < SIM_NADVERSARIES; m++) {
        simSetup su = {0.1, 0.05, &useful, &tr, NULL, simAdversaries[m].teacher, 0, SIM_A_TICK,
                       SIM_HORIZON};
        simRow row;

        simScoreRow(&su, runs, seed, 180 + m, &row);
        simWilson(row.invalid, runs, &lo, &hi);
        printf("    %-36s  %6llu  %13llu  %9.4f  [%.4f, %.4f]  %5.2f  %7.1f%%  %5.2f%% of 10%%  %6.0f  %6.2f\n",
               simAdversaries[m].name, (unsigned long long)runs, (unsigned long long)row.invalid,
               (double)row.invalid / (double)runs, lo, hi, SIM_DELTA,
               simPct(row.accepted, row.requests), simPct(row.wrong, row.accepted),
               simPer(row.late, runs), simPer(row.revocations, runs));
        CHECK(lo <= SIM_DELTA);
        CHECK(row.accepted > 0 && (double)row.wrong <= 0.1 * (double)row.accepted);
        simRowFree(&row);
    }
    printf("\n    With none of these teachers do gates answer where they should not more often than delta\n"
           "    allows. The one that never sends \"wrong\" keeps them from answering much at all: half of\n"
           "    this scorer's requests then never get an answer, and when a snapshot is promoted the hundreds\n"
           "    of them still waiting run out of time one after the other while answers have become rare\n"
           "    (only spot checks are asked for now). 32 in a row without an answer in between look like an\n"
           "    outage, and the gate stops. See (D).\n"
           "    The table took %.1f s.\n", simSeconds(start));
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
           "    delta = %.2f, audit = 0.05, %u requests per gate, every answer at once.\n"
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
            simSetup su = {alphas[a], 0.05, &simUseful[w], NULL, NULL, simAtOnce, 0, 1000, SIM_HORIZON};
            simTruth tr;
            simRow row;
            double lo, hi;

            simTruthOf(&simUseful[w], alphas[a], &tr);
            CHECK(tr.best >= 0);
            su.truth = &tr;
            simScoreRow(&su, runs, seed, 200 + a * 10 + w, &row);
            simWilson(row.invalid, runs, &lo, &hi);
            printf("    %.2f   %-11s  %6s %6s  %7.1f%% %7.1f%% %7.1f%%  %9s %10.3f  %6.2f%% of %2.0f%%"
                   "   %-9llu  %llu of %llu\n",
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
        double audit;
        int64_t tick_ms;
        simTeacher teacher;
        const char *traffic, *name;
        int must_stop;
    } rows[] = {
        {0.05, 1000, {SIM_PROMPT, 0, 0, 0.0}, "1 request/s", "every answer at once", 1},
        {0.20, 1000, {SIM_PROMPT, 0, 0, 0.0}, "1 request/s", "every answer at once", 1},
        {0.05, 1000, {SIM_PROMPT, 0, 0, 0.01}, "1 request/s", "1% of the answers lost", 1},
        {0.05, 1000, {SIM_PROMPT, 0, 0, 0.05}, "1 request/s", "5% of the answers lost", 1},
        {0.05, 50, {SIM_PROMPT, 0, 0, 0.0}, "20 requests/s", "every answer at once", 1},
        {0.05, 50, {SIM_PROMPT, 0, 0, 0.05}, "20 requests/s", "5% of the answers lost", 1},
        {0.05, 50, {SIM_FIXED, 10000, 0, 0.0}, "20 requests/s", "every answer after 10 s", 1},
        {0.05, 50, {SIM_PARETO, 3000, 0, 0.0}, "20 requests/s", "heavy tail, at least 3 s", 1},
        {0.05, 50, {SIM_WRONG_LATE, SIM_A_LATE, 0, 0.0}, "20 requests/s", "\"wrong\" 45 s late", 1},
        {0.05, 50, {SIM_WRONG_NEVER, 0, 0, 0.0}, "20 requests/s", "\"wrong\" never sent", 0}
    };
    clock_t start = clock();
    char b[6][32];
    simTruth tr;
    size_t r;

    gate_test_current = "table C";
    simTruthOf(&simUseful[1], 0.1, &tr);
    printf("\n(C) DRIFT: A HEALTHY GATE WHOSE SCORER BREAKS\n"
           "    Scorer 'steep', alpha = 0.10, delta = %.2f. From request %u on, every score is wrong half\n"
           "    of the time. Counted from there until the gate stops: the wrong answers it gave, and the\n"
           "    requests that went by. Gates that were not answering at the break are left out.\n\n",
           SIM_DELTA, SIM_FLIP);
    printf("    %-5s  %-13s  %-24s  %5s  %7s  %-24s  %-24s\n", "audit", "traffic", "teacher", "gates",
           "stopped", "wrong answers given", "requests until the stop");
    printf("    %-5s  %-13s  %-24s  %5s  %7s  %7s %7s %7s   %7s %7s %7s\n", "", "", "", "", "", "median",
           "p90", "max", "median", "p90", "max");
    for (r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        simSetup su = {0.1, rows[r].audit, &simUseful[1], &tr, &broken, rows[r].teacher, 0,
                       rows[r].tick_ms, SIM_HORIZON};
        simRow row;
        uint64_t n;

        if (r == 4) printf("\n");
        simScoreRow(&su, runs, seed, 300 + r, &row);
        n = row.active_at_flip;
        CHECK(n * 2 > runs);
        if (rows[r].must_stop) CHECK(row.stopped == n);
        if (n) {
            printf("    %.2f   %-13s  %-24s  %5llu  %7llu  %7s %7s %7s   %7s %7s %7s\n", rows[r].audit,
                   rows[r].traffic, rows[r].name, (unsigned long long)n,
                   (unsigned long long)row.stopped,
                   simFmt(b[0], 32, simQuantile(row.flip_wrong, n, 0.5)),
                   simFmt(b[1], 32, simQuantile(row.flip_wrong, n, 0.9)),
                   simFmt(b[2], 32, row.flip_wrong[n - 1]),
                   simFmt(b[3], 32, simQuantile(row.flip_requests, n, 0.5)),
                   simFmt(b[4], 32, simQuantile(row.flip_requests, n, 0.9)),
                   simFmt(b[5], 32, row.flip_requests[n - 1]));
        }
        simRowFree(&row);
    }
    printf("\n    The drift detector hears every spot check the moment its answer arrives, whatever older\n"
           "    answers are still out. A lost answer therefore delays the alarm only by being one spot check\n"
           "    fewer, and a slow teacher by its own slowness (10 seconds are 200 requests at twenty per\n"
           "    second, 45 seconds are 900). The detector only hears answers: a teacher who never sends\n"
           "    \"wrong\" is not heard at all, the spot checks it does answer end every run of missing ones,\n"
           "    and such a gate goes on answering to the end of the run ('never'), wrong half of the time.\n"
           "    The table took %.1f s.\n", simSeconds(start));
}

/* --------------------------------- (D) ---------------------------------- */

static void simLostFeedback(uint64_t runs, uint64_t seed) {
    static const struct {
        int64_t tick_ms;
        double loss;
        const char *traffic, *lost;
    } rows[] = {
        {1000, 0.0, "1 request/s", "nothing"},    {1000, 0.01, "1 request/s", "1%"},
        {1000, 0.05, "1 request/s", "5%"},        {50, 0.0, "20 requests/s", "nothing"},
        {50, 0.01, "20 requests/s", "1%"},        {50, 0.05, "20 requests/s", "5%"},
        {5, 0.0, "200 requests/s", "nothing"},    {5, 0.01, "200 requests/s", "1%"},
        {5, 0.05, "200 requests/s", "5%"},        {50, 0.2, "20 requests/s", "20%"}
    };
    static const struct {
        double audit, loss;
        const char *lost;
    } low[] = {{0.05, 0.1, "10%"}, {0.05, 0.15, "15%"}, {0.01, 0.1, "10%"}, {0.01, 0.15, "15%"}};
    clock_t start = clock();
    char b1[32], b2[32], b3[32];
    uint64_t slow_first[3] = {0, 0, 0};
    simTruth tr, tr2;
    size_t r;

    gate_test_current = "table D";
    simTruthOf(&simUseful[1], 0.1, &tr);
    printf("\n(D) LOST ANSWERS: REQUESTS THAT NEVER GET THEIR GLEARN\n"
           "    Scorer 'steep', alpha = 0.10, delta = %.2f, audit = 0.05, %u requests per gate. Of the\n"
           "    answers the gate asked for, the given share is never sent; the others arrive at once. Proof\n"
           "    waits 15 seconds for a missing answer: 15 requests at one request per second, 300 at twenty,\n"
           "    3000 at two hundred. After a minute the request is a wrong guess for good.\n\n",
           SIM_DELTA, SIM_HORIZON);
    printf("    %-14s  %-7s  %-8s  %-13s  %-18s  %-9s  %-8s  %-8s  %-8s  %s\n", "traffic", "answers",
           "time", "first ACCEPT", "share of requests", "threshold", "wrong", "stops", "counted", "refused:");
    printf("    %-14s  %-7s  %-8s  %6s %6s  %8s %9s  %-9s  %-8s  %-8s  %-8s  %s\n", "", "lost", "ACTIVE",
           "median", "p90", "answered", "the end", "the end", "answered", "per gate", "as wrong",
           "gate full");
    for (r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        simSetup su = {0.1, 0.05, &simUseful[1], &tr, NULL, {SIM_PROMPT, 0, 0, 0.0}, 0,
                       rows[r].tick_ms, SIM_HORIZON};
        uint64_t median;
        simRow row;

        su.teacher.loss = rows[r].loss;
        if (r % 3 == 0 && r) printf("\n");
        simScoreRow(&su, runs, seed, 400 + r, &row);
        median = simQuantile(row.first, runs, 0.5);
        printf("    %-14s  %-7s  %6.1f%%   %6s %6s  %7.1f%% %8.1f%%  %-9s  %6.2f%%   %8.2f  %8.1f  %7.1f%%\n",
               rows[r].traffic, rows[r].lost, simPct(row.active, row.requests),
               simFmt(b1, sizeof(b1), median),
               simFmt(b2, sizeof(b2), simQuantile(row.first, runs, 0.9)),
               simPct(row.accepted, row.requests), simPct(row.tail_accepted, runs * SIM_TAIL),
               simMedianThreshold(b3, sizeof(b3), &row), simPct(row.wrong, row.accepted),
               simPer(row.revocations, runs), simPer(row.missing, runs),
               simPct(row.full, row.requests));
        CHECK((double)row.wrong <= 0.1 * (double)row.accepted);
        CHECK(row.full == 0);
        /* What a lost answer costs in wrong guesses does not depend on the
         * request rate; the wait does, and it is the 15 seconds, not the
         * minute: against the same loss at one request per second, where
         * 15 seconds are 15 requests, the first ACCEPT comes at most two
         * grace periods of requests later. */
        if (r < 3) slow_first[r] = median;
        else if (r < 9 && rows[r].loss > 0.0)
            CHECK(median <= slow_first[r % 3] + 2 * (uint64_t)(SIM_GRACE / rows[r].tick_ms));
        /* Lost answers are wrong guesses: a share of them above alpha
         * leaves nothing to prove. */
        if (rows[r].loss > 0.1) CHECK(row.ever_active == 0);
        simRowFree(&row);
    }
    printf("\n    'time ACTIVE' is the share of requests asked while a generation was answering. 'stops' are\n"
           "    the times a gate stopped answering, by drift alarm or because 32 answers in a row were\n"
           "    missing; 'counted as wrong' the answers per gate that never came: wrong guesses to the proof,\n"
           "    nothing to the drift detector. What a lost answer costs before the first ACCEPT is the wait:\n"
           "    while a request without its answer is younger than 15 seconds the proof behind it waits, so\n"
           "    at a high request rate the first ACCEPT comes about 15 seconds of requests later, however few\n"
           "    answers are lost. After that only spot checks need answers. A share of lost answers above\n"
           "    alpha keeps a gate from ever answering: each of them is a wrong guess.\n\n");

    /* A higher alpha leaves room for more lost answers. Then the gate can
     * stop for another reason. */
    simTruthOf(&simUseful[1], 0.2, &tr2);
    printf("    The same scorer with alpha = 0.20, twenty requests per second:\n\n");
    printf("    %-5s  %-7s  %-8s  %-13s  %-8s  %-8s  %-8s\n", "audit", "answers", "time", "first ACCEPT",
           "share", "stops", "counted");
    printf("    %-5s  %-7s  %-8s  %6s %6s  %-8s  %-8s  %-8s\n", "", "lost", "ACTIVE", "median", "p90",
           "answered", "per gate", "as wrong");
    for (r = 0; r < sizeof(low) / sizeof(low[0]); r++) {
        simSetup su = {0.2, low[r].audit, &simUseful[1], &tr2, NULL, {SIM_PROMPT, 0, 0, 0.0}, 0, 50,
                       SIM_HORIZON};
        simRow row;

        su.teacher.loss = low[r].loss;
        simScoreRow(&su, runs, seed, 450 + r, &row);
        printf("    %.2f   %-7s  %6.1f%%   %6s %6s  %7.1f%%  %8.2f  %8.1f\n", low[r].audit, low[r].lost,
               simPct(row.active, row.requests),
               simFmt(b1, sizeof(b1), simQuantile(row.first, runs, 0.5)),
               simFmt(b2, sizeof(b2), simQuantile(row.first, runs, 0.9)),
               simPct(row.accepted, row.requests), simPer(row.revocations, runs),
               simPer(row.missing, runs));
        CHECK((double)row.wrong <= 0.2 * (double)row.accepted);
        simRowFree(&row);
    }
    printf("\n    The scorer does not drift here: these stops are gates that took a run of missing answers for\n"
           "    an outage. While a snapshot is tested every request needs its answer, afterwards only the spot\n"
           "    checks do. Right after a promotion the lost answers of the last minute (180 of them at 15%%)\n"
           "    run out of time one by one while answers have become rare: with 1%% spot checks, 32 in a row\n"
           "    without an answer in between happen, the gate stops, and it has to prove itself again.\n"
           "    The table took %.1f s.\n", simSeconds(start));
}

/* --------------------------------- (E) ---------------------------------- */

static void simLatency(uint64_t runs, uint64_t seed) {
    static const struct {
        simTeacher teacher;
        const char *name;
    } rows[] = {
        {{SIM_PROMPT, 0, 0, 0.0}, "at once"},
        {{SIM_FIXED, 2000, 0, 0.0}, "after 2 s, in order"},
        {{SIM_FIXED, 30000, 0, 0.0}, "after 30 s, in order"},
        {{SIM_UNIFORM, 0, 10000, 0.0}, "after 0 to 10 s"},
        {{SIM_UNIFORM, 0, 40000, 0.0}, "after 0 to 40 s"},
        {{SIM_PARETO, 500, 0, 0.0}, "heavy tail, at least 0.5 s"},
        {{SIM_PARETO, 3000, 0, 0.0}, "heavy tail, at least 3 s"}
    };
    clock_t start = clock();
    char b1[32], b2[32], b3[32];
    uint64_t prompt_first = 0;
    simTruth tr;
    size_t r;

    gate_test_current = "table E";
    simTruthOf(&simUseful[1], 0.1, &tr);
    printf("\n(E) A SLOW TEACHER: WHAT WAITING FOR LATE ANSWERS COSTS\n"
           "    Scorer 'steep', alpha = 0.10, delta = %.2f, audit = 0.05, twenty requests per second, %u\n"
           "    requests per gate. 'heavy tail': an answer takes at least the given time and longer than x\n"
           "    with probability (minimum / x)^1.5; with a minimum of 0.5 s that is 0.6%% of the answers later\n"
           "    than 15 seconds and 0.08%% later than a minute, with 3 s it is 9%% and 1.1%%.\n\n",
           SIM_DELTA, SIM_HORIZON);
    printf("    %-26s  %-13s  %-8s  %-17s  %-9s  %-8s  %-31s  %s\n", "the answer arrives",
           "first ACCEPT", "time", "share of requests", "threshold", "wrong", "per gate: answers that were",
           "bad threshold");
    printf("    %-26s  %6s %6s  %-8s  %8s %8s  %-9s  %-8s  %9s %9s %11s  %s\n", "", "median", "p90",
           "ACTIVE", "answered", "the end", "the end", "answered", "set right", "too late", "never used",
           "(gates)");
    for (r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        simSetup su = {0.1, 0.05, &simUseful[1], &tr, NULL, rows[r].teacher, 0, 50, SIM_HORIZON};
        uint64_t median;
        double lo, hi;
        simRow row;

        simScoreRow(&su, runs, seed, 600 + r, &row);
        median = simQuantile(row.first, runs, 0.5);
        simWilson(row.invalid, runs, &lo, &hi);
        printf("    %-26s  %6s %6s  %6.1f%%   %7.1f%% %7.1f%%  %-9s  %6.2f%%   %9.1f %9.1f %11.1f  %llu of %llu\n",
               rows[r].name, simFmt(b1, sizeof(b1), median),
               simFmt(b2, sizeof(b2), simQuantile(row.first, runs, 0.9)),
               simPct(row.active, row.requests), simPct(row.accepted, row.requests),
               simPct(row.tail_accepted, runs * SIM_TAIL), simMedianThreshold(b3, sizeof(b3), &row),
               simPct(row.wrong, row.accepted), simPer(row.late, runs), simPer(row.refused, runs),
               simPer(row.missing, runs), (unsigned long long)row.invalid, (unsigned long long)runs);
        CHECK(lo <= SIM_DELTA);
        CHECK(row.accepted > 0 && (double)row.wrong <= 0.1 * (double)row.accepted);
        if (r == 0) prompt_first = median;
        /* Answers that come in order cost their latency and nothing else:
         * nothing answered ever waits behind an open request. */
        if (rows[r].teacher.kind == SIM_FIXED) {
            CHECK(row.late == 0);
            CHECK(median <= prompt_first + (uint64_t)(rows[r].teacher.lo / 50) + 50);
        }
        simRowFree(&row);
    }
    printf("\n    'set right': answers that arrived after proof had moved on without them (the request was\n"
           "    counted as a wrong guess for the time being) and were used. 'too late': answers that took a\n"
           "    minute or more and were refused. 'never used': requests that stayed wrong guesses to the proof\n"
           "    because their answer was too late. Answers that come in request order cost their own delay:\n"
           "    30 seconds are 600 requests. Answers out of order make proof wait for the slowest of them,\n"
           "    up to 15 seconds (300 requests), and count the ones that are slower still as wrong guesses\n"
           "    until they arrive.\n"
           "    The table took %.1f s.\n", simSeconds(start));
}

/* --------------------------------- (F) ---------------------------------- */

static void simOutage(uint64_t runs, uint64_t seed) {
    static const struct {
        int64_t tick_ms;
        uint64_t requests;
        const char *traffic, *length;
        int must_stop;
    } rows[] = {
        {1000, 300, "1 request/s", "5 minutes", 0},   {1000, 600, "1 request/s", "10 minutes", 0},
        {1000, 1200, "1 request/s", "20 minutes", 1}, {1000, 3600, "1 request/s", "1 hour", 1},
        {50, 1200, "20 requests/s", "1 minute", 0},   {50, 2400, "20 requests/s", "2 minutes", 0},
        {50, 6000, "20 requests/s", "5 minutes", 1}
    };
    clock_t start = clock();
    char b[5][32];
    simTruth tr;
    size_t r;

    gate_test_current = "table F";
    simTruthOf(&simUseful[1], 0.1, &tr);
    printf("\n(F) A FEEDBACK OUTAGE: NO ANSWER ARRIVES FOR A WHILE\n"
           "    Scorer 'steep', alpha = 0.10, delta = %.2f, audit = 0.05, %u requests per gate. From\n"
           "    request %u on no answer arrives for the given time; before and after, every answer at\n"
           "    once. Gates that were not answering when the outage began are left out of the columns about\n"
           "    stopping.\n\n", SIM_DELTA, SIM_HORIZON, SIM_FLIP);
    printf("    %-14s  %-10s  %-9s  %-7s  %-18s  %-12s  %-20s  %-21s  %-8s\n", "traffic", "outage",
           "answering", "stopped", "stopped after", "answers", "answering again after", "snapshot numbers",
           "wrong");
    printf("    %-14s  %-10s  %-9s  %-7s  %8s %9s  %-12s  %9s %10s  %10s %10s  %-8s\n", "", "", "at start",
           "", "requests", "seconds", "meanwhile", "median", "p90", "in outage", "by the end", "answered");
    for (r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        simSetup su = {0.1, 0.05, &simUseful[1], &tr, NULL, simAtOnce, rows[r].requests,
                       rows[r].tick_ms, SIM_HORIZON};
        uint64_t n, stop, nstop;
        simRow row;

        if (r == 4) printf("\n");
        simScoreRow(&su, runs, seed, 700 + r, &row);
        n = row.active_at_flip;
        nstop = row.out_stopped;
        /* Among the gates that stopped: the median of when. */
        stop = nstop ? simQuantile(row.out_stop, nstop, 0.5) : SIM_NEVER;
        printf("    %-14s  %-10s  %9llu  %7llu  %8s %9s  %12s  %9s %10s  %10llu %10.2f  %6.2f%%\n",
               rows[r].traffic, rows[r].length, (unsigned long long)n, (unsigned long long)nstop,
               simFmt(b[0], 32, stop),
               stop == SIM_NEVER ? "-" : simFmt(b[1], 32, stop * (uint64_t)rows[r].tick_ms / 1000),
               n ? simFmt(b[2], 32, simQuantile(row.out_accepted, n, 0.5)) : "-",
               n ? simFmt(b[3], 32, simQuantile(row.out_back, n, 0.5)) : "-",
               n ? simFmt(b[4], 32, simQuantile(row.out_back, n, 0.9)) : "-",
               (unsigned long long)row.out_generations, simPer(row.end_generations, runs),
               simPct(row.wrong, row.accepted));
        CHECK(n * 2 > runs);
        /* An outage uses no snapshot number while it lasts. */
        CHECK(row.out_generations == 0);
        /* A long one stops every gate, by the gate seeing that it is blind;
         * and every gate answers again before the run is over. */
        if (rows[r].must_stop) {
            CHECK(nstop == n && row.out_blind == n);
            CHECK(n && simQuantile(row.out_back, n, 1.0) != SIM_NEVER);
        }
        CHECK((double)row.wrong <= 0.1 * (double)row.accepted);
        simRowFree(&row);
    }
    printf("\n    A gate cannot tell an outage from a slow teacher until the answers it asked for have been\n"
           "    missing for a minute, and it takes 32 of them in a row before it stops: at 5%% spot checks\n"
           "    that is about 640 requests plus the minute. Until then it goes on answering unchecked\n"
           "    ('answers meanwhile', the median over the gates). An outage shorter than that costs wealth\n"
           "    (every spot check of the outage is a wrong guess to the proof) and nothing else. A gate that\n"
           "    stopped keeps what it has and takes no snapshot until an answer arrives: 'in outage' is the\n"
           "    largest number of snapshot numbers any gate used during the outage, 'by the end' the mean\n"
           "    from its beginning to the end of the run. 'answering again after' counts requests from the\n"
           "    end of the outage to the next ACCEPT; gates that never stopped answer again at once.\n"
           "    The table took %.1f s.\n", simSeconds(start));
}

/* --------------------------------- (G) ---------------------------------- */

static void simClassifier(uint64_t runs, uint64_t seed) {
    static const char *const names[] = {"pure noise", "learnable"};
    enum { OUTAGE = 1800 };
    clock_t start = clock();
    char b1[32], b2[32], b3[32], b4[32];
    uint64_t scratch = 0, i, n;
    simRow row;
    int learnable;

    gate_test_current = "table G";
    printf("\n(G) CLASSIFIER GATE: TWO LABELS, FOUR WORDS PER REQUEST\n"
           "    alpha = 0.10, delta = %.2f, audit = 0.05, a snapshot every 1000 examples, %u requests per\n"
           "    gate, one per second, every answer at once. 'learnable': the label is the topic the words\n"
           "    were drawn for (about 11%% of the requests are ambiguous even to a perfect reader). 'pure\n"
           "    noise': the label is a coin flip that has nothing to do with the words.\n\n", SIM_DELTA,
           SIM_HORIZON);
    printf("    %-10s  %5s  %-34s  %-13s  %-18s  %-11s  %s\n", "problem", "gates", "gates that ever answered",
           "first ACCEPT", "share of requests", "snapshots", "wrong among");
    printf("    %-10s  %5s  %6s %9s  %-16s  %6s %6s  %8s %9s  %-11s  %s\n", "", "", "", "frequency",
           "95% interval", "median", "p90", "answered", "the end", "guess wrong", "the answered");
    for (learnable = 0; learnable < 2; learnable++) {
        double lo, hi;

        simRowInit(&row, runs);
        for (i = 0; i < runs; i++) {
            simResult res;

            simClassRun(learnable, 0.1, 0, simStream(seed, (500 + (uint64_t)learnable) * 1000003ull + i),
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
            scratch = simQuantile(row.first, runs, 0.5);
        } else {
            CHECK(lo <= SIM_DELTA); /* a gate trained on noise must not answer */
        }
        simRowFree(&row);
    }

    /* The learnable problem through an outage. */
    simRowInit(&row, runs);
    for (i = 0; i < runs; i++) {
        simResult res;

        simClassRun(1, 0.1, OUTAGE, simStream(seed, 502 * 1000003ull + i), &res);
        simRowAdd(&row, &res);
    }
    simRowClose(&row);
    n = row.active_at_flip;
    printf("\n    The learnable problem again, with no answer arriving for 30 minutes from request %u on:\n"
           "    of %llu gates that were answering then, %llu stopped (after %s requests, the median) and gave\n"
           "    %s answers meanwhile; none used a snapshot number during the outage (the most: %llu) and %llu\n"
           "    of %llu lost what their model had learned. They answered again %s requests after the outage\n"
           "    (the median; 90%% within %s), where a gate that starts from nothing needs %llu for its first\n"
           "    ACCEPT. Wrong among the answered: %.2f%% of 10%%.\n",
           SIM_FLIP, (unsigned long long)n, (unsigned long long)row.out_stopped,
           n ? simFmt(b1, sizeof(b1), simQuantile(row.out_stop, n, 0.5)) : "-",
           n ? simFmt(b2, sizeof(b2), simQuantile(row.out_accepted, n, 0.5)) : "-",
           (unsigned long long)row.out_generations, (unsigned long long)row.model_lost,
           (unsigned long long)runs, n ? simFmt(b3, sizeof(b3), simQuantile(row.out_back, n, 0.5)) : "-",
           n ? simFmt(b4, sizeof(b4), simQuantile(row.out_back, n, 0.9)) : "-",
           (unsigned long long)scratch, simPct(row.wrong, row.accepted));
    CHECK(n * 2 > runs);
    CHECK(row.out_stopped == n && row.out_blind == n);
    CHECK(row.out_generations == 0);
    /* Missing answers say nothing about the model: every gate kept its own. */
    CHECK(row.model_lost == 0);
    CHECK(n && simQuantile(row.out_back, n, 0.5) < scratch);
    CHECK((double)row.wrong <= 0.1 * (double)row.accepted);
    simRowFree(&row);
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
    simLatency(runs, seed);
    simOutage(runs, seed);
    simClassifier(runs, seed);
    printf("\nTotal CPU time %.1f s. Consistency checks: ", simSeconds(start));
    return TEST_REPORT();
}
