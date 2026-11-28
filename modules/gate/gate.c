/*
 * Gate: a Redis data type that learns when a cheap answer is good enough.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * This file is the Redis side of the gate: commands, replies, persistence
 * and replication. What a gate does is in gate_core.c; SPEC.md describes
 * the commands.
 *
 * ============================ How this file works ==========================
 *
 * REPLICATION. A gate rolls dice and reads the clock, so its commands must
 * not be re-executed by replicas or by the AOF. Every public command
 * therefore does three things: it REALIZES whatever is not deterministic
 * (the clock, the random draw, the guesses of the frozen models), APPLIES
 * the realized operation through gate_core.c, and replicates that realized
 * operation as the internal command
 *
 *     _GATE.APPLY key <operation> <realized values> [original arguments]
 *
 * which replicas and the AOF loader apply by calling the same gate_core.c
 * function with the same values. The internal command is flagged
 * "internal": clients cannot call it.
 *
 * An ACCEPT changes nothing that replicas need (only request counters kept
 * by the primary), so it replicates nothing: the fast path never touches
 * the AOF or the replication stream.
 *
 * RECOVERY. After a restart, a failover or a RESTORE, Redis cannot show
 * that the gate lost nothing (asynchronous replication, AOF fsync
 * policies). Proof that may be incomplete is not proof, so a gate is only
 * trusted by the server incarnation that created it or last recovered it:
 * the first write command that finds an untrusted gate makes it drop its
 * proof and its waiting requests and start proving itself again
 * (gateRecover), and replicates that. 'runtime_tag' holds the incarnation
 * that trusts the gate; it is never saved, so everything loaded from disk,
 * received from a primary or restored from a dump starts untrusted.
 */

#include "../../src/redismodule.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gate_core.h"
#include "gate_features.h"
#include "gate_args.h"
#include "gate_codec.h"

static RedisModuleType *GateType;

/* Server configuration, read when a gate is created. (gate.test-seed is
 * for tests: Redis does not call a setter for an unchanged value, so to
 * restart the sequence set it to 0 and then to the seed again.) */
static long long gateCfgSnapshotExamples = GATE_DEFAULT_SNAPSHOT_EXAMPLES;
static long long gateCfgFeedbackTimeout = GATE_DEFAULT_FEEDBACK_TIMEOUT_MS;
static long long gateCfgTestSeed = 0;

/* The incarnation of this server as a primary. Changes at every role
 * change, so gates trusted before a failover are untrusted after it. */
static uint64_t gateIncarnation;

/* Apply operations that did not fit the gate they were sent to. Should
 * stay 0; anything else means a replica diverged from its primary. Shown
 * as gate_apply_mismatches in INFO. */
static unsigned long long gateApplyMismatches;

/* A similarity computed in floating point can land a hair outside [0,1]
 * (1.0000001): that is rounding, not a mistake, and is clamped. Anything
 * further out is a caller error. */
#define GATE_SCORE_SLACK 1e-6

/* ------------------------------ Randomness ------------------------------- */

static uint64_t gateTestState;

/* Random 64 bits. Normally from the server's generator, a few draws per
 * call to it; from a seeded sequence when gate.test-seed is set, so that
 * tests can be exact. */
static uint64_t gateRandom64(void) {
    static unsigned char pool[32];
    static int left = 0;
    uint64_t v;

    if (gateCfgTestSeed) {
        uint64_t z = (gateTestState += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    if (left == 0) {
        RedisModule_GetRandomBytes(pool, sizeof(pool));
        left = sizeof(pool) / sizeof(v);
    }
    left--;
    memcpy(&v, pool + left * sizeof(v), sizeof(v));
    return v;
}

/* Uniform in [0,1) with 53 random bits. */
static double gateRandomUniform(void) {
    return (double)(gateRandom64() >> 11) * (1.0 / 9007199254740992.0);
}

/* A new incarnation: always really random, even under gate.test-seed (a
 * repeated value would make this server trust proof it must not), and
 * never 0, which is what loaded gates carry. */
static uint64_t gateNewIncarnation(void) {
    uint64_t v;

    RedisModule_GetRandomBytes((unsigned char *)&v, sizeof(v));
    return v | 1;
}

/* ------------------------------- Arguments ------------------------------- */

#define GATE_STACK_ARGS 48

/* View argv[0..argc) as (pointer, length) pairs. Uses 'stack' when it
 * fits; otherwise the caller frees the result with RedisModule_Free. */
static gateArg *gateArgsView(RedisModuleString **argv, int argc, gateArg *stack) {
    gateArg *args = stack;

    if (argc > GATE_STACK_ARGS) args = RedisModule_Alloc(sizeof(gateArg) * (size_t)argc);
    for (int i = 0; i < argc; i++) args[i].ptr = RedisModule_StringPtrLen(argv[i], &args[i].len);
    return args;
}

static void gateArgsRelease(gateArg *args, gateArg *stack) {
    if (args != stack) RedisModule_Free(args);
}

/* A decimal integer that may be negative (label indices use -1). */
static int gateParseI64(RedisModuleString *s, long long *out) {
    return RedisModule_StringToLongLong(s, out) == REDISMODULE_OK;
}

static int gateStringHex64(RedisModuleString *s, uint64_t *out) {
    size_t len;
    const char *p = RedisModule_StringPtrLen(s, &len);
    return gateParseHex64(p, len, out);
}

static int gateStringDoubleBits(RedisModuleString *s, double *out) {
    size_t len;
    const char *p = RedisModule_StringPtrLen(s, &len);
    return gateParseDoubleBits(p, len, out);
}

/* --------------------------------- Keys ---------------------------------- */

/* Open an existing gate. On failure replies with the error and returns
 * REDISMODULE_ERR; '*key' is closed. */
static int gateOpen(RedisModuleCtx *ctx, RedisModuleString *name, int mode,
                    RedisModuleKey **key, gate **g)
{
    *key = RedisModule_OpenKey(ctx, name, mode);
    if (*key == NULL || RedisModule_KeyType(*key) == REDISMODULE_KEYTYPE_EMPTY) {
        RedisModule_CloseKey(*key);
        RedisModule_ReplyWithError(ctx, "ERR no such key");
        return REDISMODULE_ERR;
    }
    if (RedisModule_ModuleTypeGetType(*key) != GateType) {
        RedisModule_CloseKey(*key);
        RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
        return REDISMODULE_ERR;
    }
    *g = RedisModule_ModuleTypeGetValue(*key);
    return REDISMODULE_OK;
}

/* The clock a command uses. The first time this incarnation uses a gate
 * it is the wall clock, never behind the gate; from then on the gate's own
 * clock advanced by the monotonic time that has passed. A wall clock that
 * is stepped forward for a moment and then corrected can therefore neither
 * rewind a gate nor freeze it until real time catches up. */
static long long gateNow(gate *g) {
    long long mono = (long long)(RedisModule_MonotonicMicroseconds() / 1000);
    long long now;

    if (g->runtime_tag != gateIncarnation || g->runtime_clock == 0) {
        now = RedisModule_Milliseconds();
        if (now < g->now_ms) now = g->now_ms;
    } else {
        long long elapsed = mono - g->runtime_clock;

        /* The top of the range is centuries away, but a gate restored
         * from a forged payload may sit right below it: add without
         * passing it. */
        if (elapsed <= 0) now = g->now_ms;
        else if (elapsed > GATE_MAX_CLOCK_MS - g->now_ms) now = GATE_MAX_CLOCK_MS;
        else now = g->now_ms + elapsed;
    }
    g->runtime_clock = mono > 0 ? mono : 1;
    return now > GATE_MAX_CLOCK_MS ? GATE_MAX_CLOCK_MS : now;
}

/* Make sure this incarnation trusts the gate; see RECOVERY above. */
static void gateTrust(RedisModuleCtx *ctx, RedisModuleString *key, gate *g, long long now) {
    char hex[17];

    if (g->runtime_tag == gateIncarnation) return;
    uint64_t epoch = gateRandom64();
    gateRecover(g, now, epoch);
    g->runtime_tag = gateIncarnation;
    gateFormatHex64(hex, epoch);
    RedisModule_Replicate(ctx, "_GATE.APPLY", "sclc", key, "RECOVER", now, hex);
}

/* The gate changed in a way a reader could see: WATCH and client-side
 * caches must know. */
static void gateChanged(RedisModuleCtx *ctx, RedisModuleString *key) {
    RedisModule_SignalModifiedKey(ctx, key);
}

static void gateReplicateTick(RedisModuleCtx *ctx, RedisModuleString *key, long long now) {
    RedisModule_Replicate(ctx, "_GATE.APPLY", "scl", key, "TICK", now);
}

/* The arguments "TEXT text ..." of a request as they go into the stream:
 * only the part of the text that is read (GATE_TEXT_READ_BYTES) travels.
 * A replica gets the same clues from it, and a megabyte of text does not
 * end up in the AOF. 'argv[0]' is the word TEXT. Returns the vector to
 * replicate ('copy' if the text was cut, else argv itself) and in '*cut'
 * a string the caller frees with RedisModule_FreeString (or NULL). */
static RedisModuleString **gateStreamRequest(RedisModuleCtx *ctx, RedisModuleString **argv,
                                             int argc, RedisModuleString **copy,
                                             RedisModuleString **cut)
{
    size_t len;
    const char *text;

    *cut = NULL;
    if (argc < 2) return argv;
    text = RedisModule_StringPtrLen(argv[1], &len);
    if (len <= GATE_TEXT_READ_BYTES) return argv;
    *cut = RedisModule_CreateString(ctx, text, GATE_TEXT_READ_BYTES);
    for (int i = 0; i < argc; i++) copy[i] = argv[i];
    copy[1] = *cut;
    return copy;
}

/* Parse "TEXT text [FEATURES ...]" at args[*pos] and turn it into clues. */
static int gateClues(const gateArg *args, int argc, int *pos, gateSparse *x,
                     char *err, size_t errlen)
{
    gateRequest req;

    if (gateParseRequest(args, argc, pos, &req, err, errlen) != GATE_OK) return GATE_ERR;
    return gateFeaturize(req.text, req.text_len, req.features, req.nfeatures, x, err, errlen);
}

/* -------------------------------- GCREATE -------------------------------- */

/* GCREATE key ALPHA alpha <CLASSES count label ... | SCORE> [options] */
static int GCreate_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    gateArg stack[GATE_STACK_ARGS], *args;
    gateConfig cfg;
    char err[160], lineage_hex[17], epoch_hex[17];

    if (argc < 5) return RedisModule_WrongArity(ctx);
    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
    if (RedisModule_KeyType(key) != REDISMODULE_KEYTYPE_EMPTY) {
        RedisModule_CloseKey(key);
        return RedisModule_ReplyWithError(ctx, "BUSYKEY Target key name already exists.");
    }

    args = gateArgsView(argv + 2, argc - 2, stack);
    int ok = gateParseCreate(args, argc - 2, (uint32_t)gateCfgSnapshotExamples,
                             gateCfgFeedbackTimeout, &cfg, err, sizeof(err)) == GATE_OK;
    gateArgsRelease(args, stack);
    if (!ok) {
        RedisModule_CloseKey(key);
        return RedisModule_ReplyWithErrorFormat(ctx, "ERR %s", err);
    }

    long long now = RedisModule_Milliseconds();
    uint64_t lineage = gateRandom64(), epoch = gateRandom64();
    gate *g = gateCreate(&cfg, lineage, epoch, now);
    if (g == NULL) {
        gateConfigFree(&cfg);
        RedisModule_CloseKey(key);
        return RedisModule_ReplyWithError(ctx, "ERR out of memory");
    }
    g->runtime_tag = gateIncarnation;
    (void)gateNow(g); /* starts the gate's clock */
    RedisModule_ModuleTypeSetValue(key, GateType, g);
    RedisModule_CloseKey(key);
    gateChanged(ctx, argv[1]);

    gateFormatHex64(lineage_hex, lineage);
    gateFormatHex64(epoch_hex, epoch);
    RedisModule_Replicate(ctx, "_GATE.APPLY", "sclccllv", argv[1], "CREATE", now,
                          lineage_hex, epoch_hex, (long long)cfg.snapshot_examples,
                          (long long)cfg.feedback_timeout_ms, argv + 2, (size_t)(argc - 2));
    gateConfigFree(&cfg);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* -------------------------------- GPREDICT ------------------------------- */

/* GPREDICT key TEXT text [FEATURES count name value ...]
 * GPREDICT key SCORE score */
static int GPredict_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    gateArg stack[GATE_STACK_ARGS], *args;
    RedisModuleKey *key;
    gate *g;
    gateSparse x = {0};
    gateEval ev;
    gatePredictResult res;
    char err[160];
    double score = 0;

    if (argc < 4) return RedisModule_WrongArity(ctx);
    if (gateOpen(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE, &key, &g) != REDISMODULE_OK)
        return REDISMODULE_OK;
    int classifier = g->cfg.kind == GATE_KIND_CLASSIFIER;

    /* Everything about the arguments is checked before the gate changes. */
    args = gateArgsView(argv, argc, stack);
    int ok = 1;
    if (classifier) {
        int pos = 2;
        if (!gateArgIs(&args[2], "TEXT")) {
            snprintf(err, sizeof(err), "this gate is a classifier: GPREDICT key TEXT text "
                                       "[FEATURES count name value ...]");
            ok = 0;
        } else if (gateClues(args, argc, &pos, &x, err, sizeof(err)) != GATE_OK) {
            ok = 0;
        } else if (pos != argc) {
            snprintf(err, sizeof(err), "syntax error");
            ok = 0;
        }
    } else {
        if (argc != 4 || !gateArgIs(&args[2], "SCORE")) {
            snprintf(err, sizeof(err), "this is a score gate: GPREDICT key SCORE score");
            ok = 0;
        } else if (!gateParseDouble(args[3].ptr, args[3].len, &score) ||
                   score < -GATE_SCORE_SLACK || score > 1.0 + GATE_SCORE_SLACK) {
            snprintf(err, sizeof(err), "SCORE must be a number between 0 and 1");
            ok = 0;
        } else if (score < 0.0) {
            score = 0.0;
        } else if (score > 1.0) {
            score = 1.0;
        }
    }
    gateArgsRelease(args, stack);
    if (!ok) {
        gateSparseFree(&x);
        RedisModule_CloseKey(key);
        return RedisModule_ReplyWithErrorFormat(ctx, "ERR %s", err);
    }

    long long now = gateNow(g);
    gateTrust(ctx, argv[1], g, now);
    int due = gateMaintenanceDue(g, now);
    gateMaintain(g, now);

    double u = gateRandomUniform();
    int recorded_with_clues = 0;
    if (gateEvaluateFor(g, classifier ? &x : NULL, score, u, &ev) != GATE_OK) {
        /* A model produced a non-finite guess: do not trust it with this
         * request. Nothing was drawn or recorded. */
        res.action = GATE_ACTION_ABSTAIN;
        res.label = -1;
        res.seq = 0;
    } else {
        recorded_with_clues = gatePredictNeedsFeatures(g, u, &ev);
        if (gatePredict(g, now, u, &ev, classifier ? &x : NULL, &res) != GATE_OK) {
            res.action = GATE_ACTION_ABSTAIN;
            res.label = -1;
            res.seq = 0;
        }
    }
    gateSparseFree(&x);

    /* Only a recorded request changes what replicas hold. */
    if (res.seq != 0) {
        char ubits[17], sbits[17], cbits[17];
        RedisModuleString *cut = NULL, **stream = argv + 2;
        RedisModuleString **copy = NULL;
        size_t nstream = 0;

        if (recorded_with_clues) {
            nstream = (size_t)(argc - 2);
            copy = RedisModule_Alloc(sizeof(*copy) * nstream);
            stream = gateStreamRequest(ctx, argv + 2, argc - 2, copy, &cut);
        }
        gateFormatDoubleBits(ubits, u);
        gateFormatDoubleBits(sbits, ev.serving.score);
        gateFormatDoubleBits(cbits, ev.candidate.score);
        RedisModule_Replicate(ctx, "_GATE.APPLY", "sclcllcllclv", argv[1], "PREDICT", now,
                              ubits, (long long)ev.has_serving, (long long)ev.serving.label,
                              sbits, (long long)ev.has_candidate,
                              (long long)ev.candidate.label, cbits, (long long)res.seq,
                              stream, nstream);
        if (cut) RedisModule_FreeString(ctx, cut);
        if (copy) RedisModule_Free(copy);
    } else if (due) {
        gateReplicateTick(ctx, argv[1], now);
    }
    gateChanged(ctx, argv[1]); /* at least the counters GINFO shows moved */

    RedisModule_ReplyWithArray(ctx, 2);
    RedisModule_ReplyWithCString(ctx, gateActionName(res.action));
    if (res.action == GATE_ACTION_ACCEPT) {
        if (res.label >= 0)
            RedisModule_ReplyWithCString(ctx, g->cfg.labels[res.label]);
        else
            RedisModule_ReplyWithNull(ctx);
    } else if (res.seq != 0) {
        char id[GATE_ID_MAX];
        size_t len = gateFormatId(id, sizeof(id), g->epoch, res.seq);
        RedisModule_ReplyWithStringBuffer(ctx, id, len);
    } else {
        RedisModule_ReplyWithNull(ctx);
    }
    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

/* --------------------------------- GLEARN -------------------------------- */

/* The answer of a GLEARN / the label of a GADD: "LABEL label" for a
 * classifier, "CORRECT 0|1" for a score gate. Returns the label index or
 * the 0/1, or -1 after writing 'err'. */
static int gateAnswer(const gate *g, RedisModuleString *word, RedisModuleString *value,
                      char *err, size_t errlen)
{
    gateArg w, v;

    w.ptr = RedisModule_StringPtrLen(word, &w.len);
    v.ptr = RedisModule_StringPtrLen(value, &v.len);
    if (g->cfg.kind == GATE_KIND_CLASSIFIER) {
        if (!gateArgIs(&w, "LABEL")) {
            snprintf(err, errlen, "this gate is a classifier: expected LABEL label");
            return -1;
        }
        int label = gateConfigLabelIndex(&g->cfg, v.ptr, v.len);
        if (label < 0) snprintf(err, errlen, "unknown label");
        return label;
    }
    if (!gateArgIs(&w, "CORRECT")) {
        snprintf(err, errlen, "this is a score gate: expected CORRECT 0|1");
        return -1;
    }
    if (v.len == 1 && (v.ptr[0] == '0' || v.ptr[0] == '1')) return v.ptr[0] - '0';
    snprintf(err, errlen, "CORRECT must be 0 or 1");
    return -1;
}

/* GLEARN key id LABEL label
 * GLEARN key id CORRECT 0|1 */
static int GLearn_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModuleKey *key;
    gate *g;
    char err[160];
    uint64_t epoch, seq;
    size_t idlen;

    if (argc != 5) return RedisModule_WrongArity(ctx);
    if (gateOpen(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE, &key, &g) != REDISMODULE_OK)
        return REDISMODULE_OK;

    int answer = gateAnswer(g, argv[3], argv[4], err, sizeof(err));
    if (answer < 0) {
        RedisModule_CloseKey(key);
        return RedisModule_ReplyWithErrorFormat(ctx, "ERR %s", err);
    }
    const char *id = RedisModule_StringPtrLen(argv[2], &idlen);
    if (!gateParseId(id, idlen, &epoch, &seq)) {
        /* Not an id this gate can have issued: nothing to do. */
        RedisModule_CloseKey(key);
        return RedisModule_ReplyWithLongLong(ctx, 0);
    }

    long long now = gateNow(g);
    gateTrust(ctx, argv[1], g, now);
    int used = gateLearn(g, now, epoch, seq, answer);
    /* The id travels as the client sent it: any 64-bit sequence number
     * survives, and the replica parses it exactly as we did. */
    RedisModule_Replicate(ctx, "_GATE.APPLY", "sclsl", argv[1], "LEARN", now, argv[2],
                          (long long)answer);
    gateChanged(ctx, argv[1]);
    RedisModule_CloseKey(key);
    return RedisModule_ReplyWithLongLong(ctx, used);
}

/* ---------------------------------- GADD --------------------------------- */

/* GADD key TEXT text [FEATURES count name value ...] LABEL label */
static int GAdd_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    gateArg stack[GATE_STACK_ARGS], *args;
    RedisModuleKey *key;
    gate *g;
    gateSparse x = {0};
    char err[160];
    int label = -1, pos = 2;

    if (argc < 6) return RedisModule_WrongArity(ctx);
    if (gateOpen(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE, &key, &g) != REDISMODULE_OK)
        return REDISMODULE_OK;

    args = gateArgsView(argv, argc, stack);
    int ok = 1;
    if (g->cfg.kind != GATE_KIND_CLASSIFIER) {
        snprintf(err, sizeof(err), "GADD needs a classifier gate");
        ok = 0;
    } else if (gateClues(args, argc, &pos, &x, err, sizeof(err)) != GATE_OK) {
        ok = 0;
    } else if (pos != argc - 2) {
        snprintf(err, sizeof(err), "syntax error: expected LABEL label at the end");
        ok = 0;
    } else if ((label = gateAnswer(g, argv[pos], argv[pos + 1], err, sizeof(err))) < 0) {
        ok = 0;
    }
    gateArgsRelease(args, stack);
    if (!ok) {
        gateSparseFree(&x);
        RedisModule_CloseKey(key);
        return RedisModule_ReplyWithErrorFormat(ctx, "ERR %s", err);
    }

    long long now = gateNow(g);
    gateTrust(ctx, argv[1], g, now);
    (void)gateTrain(g, now, &x, label);
    gateSparseFree(&x);
    {
        RedisModuleString *cut = NULL;
        RedisModuleString **copy = RedisModule_Alloc(sizeof(*copy) * (size_t)(argc - 2));
        RedisModuleString **stream = gateStreamRequest(ctx, argv + 2, argc - 2, copy, &cut);

        RedisModule_Replicate(ctx, "_GATE.APPLY", "sclv", argv[1], "ADD", now, stream,
                              (size_t)(argc - 2));
        if (cut) RedisModule_FreeString(ctx, cut);
        RedisModule_Free(copy);
    }
    gateChanged(ctx, argv[1]);
    long long learned = (long long)g->trainer.updates;
    RedisModule_CloseKey(key);
    return RedisModule_ReplyWithLongLong(ctx, learned);
}

/* ---------------------------------- GINFO -------------------------------- */

/* GINFO key */
static int GInfo_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModuleKey *key;
    gate *g;

    if (argc != 2) return RedisModule_WrongArity(ctx);
    if (gateOpen(ctx, argv[1], REDISMODULE_READ, &key, &g) != REDISMODULE_OK)
        return REDISMODULE_OK;

    /* On a primary, a gate this incarnation does not trust yet will drop
     * its proof at its next write: report it as it will be, not as it was
     * saved. Replicas show what their primary told them. */
    int untrusted = (RedisModule_GetContextFlags(ctx) & REDISMODULE_CTX_FLAGS_MASTER) &&
                    g->runtime_tag != gateIncarnation;
    int classifier = g->cfg.kind == GATE_KIND_CLASSIFIER;
    const gateGeneration *serving = untrusted ? NULL : g->serving;
    const gateGeneration *candidate = untrusted ? NULL : g->candidate;
    const gateGeneration *tested = candidate ? candidate : serving;
    /* That first write also takes a snapshot, if the model has learned
     * enough and a snapshot number is left. */
    int will_test = untrusted && g->generations < GATE_COUNT_MAX &&
                    (!classifier || g->trainer.updates >= g->cfg.snapshot_examples);

    const char *state;
    if (!untrusted) {
        state = gateStateName(gateState(g));
    } else if (g->paused) {
        state = gateStateName(GATE_STATE_PAUSED);
    } else if (will_test) {
        state = gateStateName(GATE_STATE_CALIBRATING);
    } else {
        state = gateStateName(GATE_STATE_COLD);
    }

    RedisModule_ReplyWithMap(ctx, 23);
    RedisModule_ReplyWithCString(ctx, "kind");
    RedisModule_ReplyWithCString(ctx, classifier ? "classifier" : "score");
    RedisModule_ReplyWithCString(ctx, "state");
    RedisModule_ReplyWithCString(ctx, state);
    RedisModule_ReplyWithCString(ctx, "alpha");
    RedisModule_ReplyWithDouble(ctx, g->cfg.alpha);
    RedisModule_ReplyWithCString(ctx, "delta");
    RedisModule_ReplyWithDouble(ctx, g->cfg.delta);
    RedisModule_ReplyWithCString(ctx, "audit");
    RedisModule_ReplyWithDouble(ctx, g->cfg.audit);

    RedisModule_ReplyWithCString(ctx, "classes");
    RedisModule_ReplyWithArray(ctx, g->cfg.nclasses);
    for (uint32_t i = 0; i < g->cfg.nclasses; i++)
        RedisModule_ReplyWithCString(ctx, g->cfg.labels[i]);

    RedisModule_ReplyWithCString(ctx, "alphas");
    RedisModule_ReplyWithArray(ctx, g->nrows);
    for (uint32_t r = 0; r < g->nrows; r++)
        RedisModule_ReplyWithDouble(ctx, gateRowTested(g, r) ? gateRowAlpha(g, r) : 0.0);

    RedisModule_ReplyWithCString(ctx, "thresholds");
    RedisModule_ReplyWithArray(ctx, g->nrows);
    for (uint32_t r = 0; r < g->nrows; r++) {
        double t = untrusted ? -1.0 : gateRowThreshold(g, r);
        if (t < 0) RedisModule_ReplyWithNull(ctx); else RedisModule_ReplyWithDouble(ctx, t);
    }

    RedisModule_ReplyWithCString(ctx, "generation");
    RedisModule_ReplyWithLongLong(ctx, serving ? (long long)serving->gen : 0);
    RedisModule_ReplyWithCString(ctx, "candidate");
    long long candidate_number = candidate ? (long long)candidate->gen : 0;
    if (will_test) candidate_number = (long long)g->generations + 1;
    RedisModule_ReplyWithLongLong(ctx, candidate_number);
    RedisModule_ReplyWithCString(ctx, "learned");
    RedisModule_ReplyWithLongLong(ctx, (long long)g->trainer.updates);
    RedisModule_ReplyWithCString(ctx, "requests");
    RedisModule_ReplyWithLongLong(ctx, (long long)g->stats.requests);
    RedisModule_ReplyWithCString(ctx, "accepted");
    RedisModule_ReplyWithLongLong(ctx, (long long)g->stats.accepted);
    RedisModule_ReplyWithCString(ctx, "audited");
    RedisModule_ReplyWithLongLong(ctx, (long long)g->stats.audited);
    RedisModule_ReplyWithCString(ctx, "abstained");
    RedisModule_ReplyWithLongLong(ctx, (long long)g->stats.abstained);
    RedisModule_ReplyWithCString(ctx, "coverage");
    RedisModule_ReplyWithDouble(ctx, untrusted ? 0.0 : gateCoverage(g));
    RedisModule_ReplyWithCString(ctx, "audit-samples");
    RedisModule_ReplyWithLongLong(ctx, serving ? (long long)serving->audits : 0);
    RedisModule_ReplyWithCString(ctx, "audit-errors");
    RedisModule_ReplyWithLongLong(ctx, serving ? (long long)serving->audit_errors : 0);
    RedisModule_ReplyWithCString(ctx, "calibration-samples");
    RedisModule_ReplyWithLongLong(ctx, tested ? (long long)tested->samples : 0);
    RedisModule_ReplyWithCString(ctx, "pending");
    RedisModule_ReplyWithLongLong(ctx, untrusted ? 0 : (long long)gateUnanswered(g));
    RedisModule_ReplyWithCString(ctx, "missing");
    RedisModule_ReplyWithLongLong(ctx, (long long)g->stats.missing);
    RedisModule_ReplyWithCString(ctx, "revocations");
    RedisModule_ReplyWithLongLong(ctx, (long long)g->stats.revocations);
    RedisModule_ReplyWithCString(ctx, "memory");
    RedisModule_ReplyWithLongLong(ctx, (long long)gateMemoryUsage(g));

    RedisModule_CloseKey(key);
    return REDISMODULE_OK;
}

/* ------------------------ GPAUSE, GRESUME, GRESET ------------------------ */

#define GATE_ADMIN_PAUSE 0
#define GATE_ADMIN_RESUME 1
#define GATE_ADMIN_RESET 2

static int gateAdmin(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, int what) {
    RedisModuleKey *key;
    gate *g;
    char hex[17];

    if (argc != 2) return RedisModule_WrongArity(ctx);
    if (gateOpen(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE, &key, &g) != REDISMODULE_OK)
        return REDISMODULE_OK;

    long long now = gateNow(g);
    if (what == GATE_ADMIN_RESET) {
        /* A reset drops the proof anyway: no need to recover first. */
        uint64_t epoch = gateRandom64();
        gateReset(g, now, epoch);
        g->runtime_tag = gateIncarnation;
        gateFormatHex64(hex, epoch);
        RedisModule_Replicate(ctx, "_GATE.APPLY", "sclc", argv[1], "RESET", now, hex);
    } else {
        gateTrust(ctx, argv[1], g, now);
        if (what == GATE_ADMIN_PAUSE) gatePause(g, now); else gateResume(g, now);
        RedisModule_Replicate(ctx, "_GATE.APPLY", "scl", argv[1],
                              what == GATE_ADMIN_PAUSE ? "PAUSE" : "RESUME", now);
    }
    gateChanged(ctx, argv[1]);
    RedisModule_CloseKey(key);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

static int GPause_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    return gateAdmin(ctx, argv, argc, GATE_ADMIN_PAUSE);
}

static int GResume_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    return gateAdmin(ctx, argv, argc, GATE_ADMIN_RESUME);
}

static int GReset_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    return gateAdmin(ctx, argv, argc, GATE_ADMIN_RESET);
}

/* ------------------------------- _GATE.APPLY ----------------------------- */

static int gateStringIs(RedisModuleString *s, const char *word) {
    gateArg a;
    a.ptr = RedisModule_StringPtrLen(s, &a.len);
    return gateArgIs(&a, word);
}

/* Apply one realized operation; returns 0 if it did not fit the gate. */
static int gateApply(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    RedisModuleString *op = argv[2];
    long long now = 0;
    char err[160];

    if (gateStringIs(op, "RESTORE")) {
        /* _GATE.APPLY key RESTORE blob (written by the AOF rewrite) */
        size_t len;
        if (argc != 4) return 0;
        const char *blob = RedisModule_StringPtrLen(argv[3], &len);
        gate *g = gateDecode((const unsigned char *)blob, len, err, sizeof(err));
        if (g == NULL) {
            /* Only an AOF rewrite writes this operation: the file is
             * damaged. The key is lost; say so, for each one. */
            RedisModule_Log(ctx, "warning", "gate: a gate in the append only file cannot be "
                            "restored and is lost (%s)", err);
            return 0;
        }
        RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
        RedisModule_ModuleTypeSetValue(key, GateType, g);
        RedisModule_CloseKey(key);
        return 1;
    }
    /* The clock range the core works in (see TIME in gate_core.h). */
    if (argc < 4 || !gateParseI64(argv[3], &now) || now < 0 || now > GATE_MAX_CLOCK_MS) return 0;

    if (gateStringIs(op, "CREATE")) {
        /* _GATE.APPLY key CREATE now lineage epoch snapshot timeout <args> */
        gateArg stack[GATE_STACK_ARGS], *args;
        gateConfig cfg;
        uint64_t lineage, epoch;
        long long snapshot, timeout;

        if (argc < 9 || !gateStringHex64(argv[4], &lineage) || !gateStringHex64(argv[5], &epoch) ||
            !gateParseI64(argv[6], &snapshot) || !gateParseI64(argv[7], &timeout) ||
            snapshot < 1 || snapshot > GATE_MAX_SNAPSHOT_EXAMPLES ||
            timeout < 1 || timeout > GATE_MAX_FEEDBACK_TIMEOUT_MS)
            return 0;
        args = gateArgsView(argv + 8, argc - 8, stack);
        int ok = gateParseCreate(args, argc - 8, (uint32_t)snapshot, timeout, &cfg, err,
                                 sizeof(err)) == GATE_OK;
        gateArgsRelease(args, stack);
        if (!ok) return 0;
        gate *g = gateCreate(&cfg, lineage, epoch, now);
        gateConfigFree(&cfg);
        if (g == NULL) return 0;
        RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
        RedisModule_ModuleTypeSetValue(key, GateType, g);
        RedisModule_CloseKey(key);
        return 1;
    }

    RedisModuleKey *key = RedisModule_OpenKey(ctx, argv[1], REDISMODULE_READ | REDISMODULE_WRITE);
    if (RedisModule_KeyType(key) != REDISMODULE_KEYTYPE_MODULE ||
        RedisModule_ModuleTypeGetType(key) != GateType) {
        RedisModule_CloseKey(key);
        return 0;
    }
    gate *g = RedisModule_ModuleTypeGetValue(key);
    int ok = 0;

    /* Whatever this node made of the gate by itself (a writable replica),
     * it now follows somebody else's operations: it must not answer on
     * proof built that way. */
    g->runtime_tag = 0;

    if (gateStringIs(op, "PREDICT")) {
        /* _GATE.APPLY key PREDICT now u has_s s_label s_score has_c c_label
         *             c_score seq [TEXT text [FEATURES ...]] */
        int classifier = g->cfg.kind == GATE_KIND_CLASSIFIER;
        gateEval ev;
        gatePredictResult res;
        gateSparse x = {0};
        long long has_s, s_label, has_c, c_label, seq;
        double u;

        memset(&ev, 0, sizeof(ev));
        if (argc >= 12 && gateStringDoubleBits(argv[4], &u) &&
            gateParseI64(argv[5], &has_s) && gateParseI64(argv[6], &s_label) &&
            gateStringDoubleBits(argv[7], &ev.serving.score) &&
            gateParseI64(argv[8], &has_c) && gateParseI64(argv[9], &c_label) &&
            gateStringDoubleBits(argv[10], &ev.candidate.score) && gateParseI64(argv[11], &seq))
        {
            int have_clues = argc > 12;
            ev.has_serving = has_s != 0;
            ev.serving.label = (int32_t)s_label;
            ev.has_candidate = has_c != 0;
            ev.candidate.label = (int32_t)c_label;
            /* What a primary can realize for this kind of gate, checked
             * before any narrowing could hide a wild value (a NaN fails
             * every comparison). A recorded classifier request carries its
             * text, a score request never does and has no labels. The
             * request number is the primary's: taking it over keeps later
             * feedback paired with the right request even after an
             * operation that did not fit. */
            ok = u >= 0.0 && u < 1.0 &&
                 ev.serving.score >= 0.0 && ev.serving.score <= 1.0 &&
                 ev.candidate.score >= 0.0 && ev.candidate.score <= 1.0 &&
                 s_label >= -1 && s_label < GATE_MAX_CLASSES &&
                 c_label >= -1 && c_label < GATE_MAX_CLASSES &&
                 have_clues == classifier &&
                 (classifier || (s_label == -1 && c_label == -1)) &&
                 seq >= 1 && (uint64_t)seq >= g->next_seq && (uint64_t)seq < GATE_COUNT_MAX;
            if (ok && have_clues) {
                gateArg stack[GATE_STACK_ARGS], *args = gateArgsView(argv, argc, stack);
                int pos = 12;
                ok = gateClues(args, argc, &pos, &x, err, sizeof(err)) == GATE_OK && pos == argc;
                gateArgsRelease(args, stack);
            }
            /* The primary recorded this request; so must we. */
            if (ok) {
                uint64_t before = g->next_seq;

                g->next_seq = (uint64_t)seq;
                ok = gatePredict(g, now, u, &ev, have_clues ? &x : NULL, &res) == GATE_OK &&
                     res.seq == (uint64_t)seq;
                if (!ok && res.seq == 0) g->next_seq = before; /* nothing took the number */
            }
            gateSparseFree(&x);
        }
    } else if (gateStringIs(op, "LEARN")) {
        /* _GATE.APPLY key LEARN now id answer */
        uint64_t epoch, seq;
        long long answer;
        size_t idlen;
        const char *id = argc == 6 ? RedisModule_StringPtrLen(argv[4], &idlen) : NULL;

        if (id && gateParseId(id, idlen, &epoch, &seq) && gateParseI64(argv[5], &answer) &&
            answer >= 0 &&
            answer < (g->cfg.kind == GATE_KIND_CLASSIFIER ? (long long)g->cfg.nclasses : 2))
        {
            gateLearn(g, now, epoch, seq, (int32_t)answer);
            ok = 1;
        }
    } else if (gateStringIs(op, "ADD")) {
        /* _GATE.APPLY key ADD now TEXT text [FEATURES ...] LABEL label */
        gateArg stack[GATE_STACK_ARGS], *args = gateArgsView(argv, argc, stack);
        gateSparse x = {0};
        int pos = 4, label;

        if (g->cfg.kind == GATE_KIND_CLASSIFIER && argc >= 8 &&
            gateClues(args, argc, &pos, &x, err, sizeof(err)) == GATE_OK && pos == argc - 2 &&
            (label = gateAnswer(g, argv[pos], argv[pos + 1], err, sizeof(err))) >= 0)
        {
            (void)gateTrain(g, now, &x, label);
            ok = 1;
        }
        gateSparseFree(&x);
        gateArgsRelease(args, stack);
    } else if (gateStringIs(op, "PAUSE") && argc == 4) {
        gatePause(g, now);
        ok = 1;
    } else if (gateStringIs(op, "RESUME") && argc == 4) {
        gateResume(g, now);
        ok = 1;
    } else if (gateStringIs(op, "TICK") && argc == 4) {
        gateMaintain(g, now);
        ok = 1;
    } else if ((gateStringIs(op, "RESET") || gateStringIs(op, "RECOVER")) && argc == 5) {
        /* _GATE.APPLY key RESET|RECOVER now epoch */
        uint64_t epoch;

        if (gateStringHex64(argv[4], &epoch)) {
            if (gateStringIs(op, "RESET")) gateReset(g, now, epoch); else gateRecover(g, now, epoch);
            ok = 1;
        }
    }
    RedisModule_CloseKey(key);
    return ok;
}

/* _GATE.APPLY key operation ...
 *
 * Reached only from a primary's replication stream, the AOF, or an
 * internal connection. An operation that does not fit must not stop
 * replication (that would only make the divergence permanent), so it is
 * logged and counted and the reply is still OK. */
static int GApply_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    /* Too few words is one more operation that does not fit. The command is
     * registered without a minimum, so that Redis does not answer a short
     * one with an arity error of its own: an error reply to an AOF or to a
     * primary's stream can stop the server. */
    if (argc < 3 || !gateApply(ctx, argv, argc)) {
        unsigned long long n = ++gateApplyMismatches;

        if (n <= 10 || n % 1000 == 0)
            RedisModule_Log(ctx, "warning",
                            "gate: a replicated operation did not fit its gate (%llu so far); "
                            "this replica may have diverged from its primary", n);
    } else {
        gateChanged(ctx, argv[1]);
    }
    /* Module commands are never propagated implicitly: without this the
     * operation would be missing from a replica's own AOF and from its
     * sub-replicas. */
    RedisModule_ReplicateVerbatim(ctx);
    return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

/* --------------------------- Data type callbacks ------------------------- */

static void *GateRdbLoad(RedisModuleIO *io, int encver) {
    char err[160];
    size_t len;

    if (encver != GATE_CODEC_VERSION) return NULL;
    char *blob = RedisModule_LoadStringBuffer(io, &len);
    if (RedisModule_IsIOError(io)) {
        if (blob) RedisModule_Free(blob);
        return NULL;
    }
    gate *g = gateDecode((const unsigned char *)blob, len, err, sizeof(err));
    RedisModule_Free(blob);
    if (g == NULL) RedisModule_LogIOError(io, "warning", "gate: cannot load: %s", err);
    return g; /* runtime_tag is 0: untrusted until a primary recovers it */
}

static void GateRdbSave(RedisModuleIO *io, void *value) {
    unsigned char *blob;
    size_t len;

    if (gateEncode(value, 0, &blob, &len) != GATE_OK) return;
    RedisModule_SaveStringBuffer(io, (const char *)blob, len);
    gate_free(blob);
}

static void GateAofRewrite(RedisModuleIO *io, RedisModuleString *key, void *value) {
    unsigned char *blob;
    size_t len;

    if (gateEncode(value, 0, &blob, &len) != GATE_OK) return;
    RedisModule_EmitAOF(io, "_GATE.APPLY", "scb", key, "RESTORE", (const char *)blob, len);
    gate_free(blob);
}

static void GateDigest(RedisModuleDigest *md, void *value) {
    unsigned char *blob;
    size_t len;

    if (gateEncode(value, 1, &blob, &len) != GATE_OK) return;
    RedisModule_DigestAddStringBuffer(md, (const char *)blob, len);
    RedisModule_DigestEndSequence(md);
    gate_free(blob);
}

static size_t GateMemUsage(const void *value) {
    return (size_t)gateMemoryUsage(value);
}

static size_t GateFreeEffort(RedisModuleString *key, const void *value) {
    REDISMODULE_NOT_USED(key);
    return (size_t)gateFreeEffort(value);
}

static void GateFree(void *value) {
    gateFree(value);
}

/* ------------------------------ Server events ---------------------------- */

static void gateRoleChanged(RedisModuleCtx *ctx, RedisModuleEvent e, uint64_t sub, void *data) {
    REDISMODULE_NOT_USED(ctx);
    REDISMODULE_NOT_USED(e);
    REDISMODULE_NOT_USED(sub);
    REDISMODULE_NOT_USED(data);
    /* Whatever this server becomes, gates trusted so far are not trusted
     * by the new incarnation. */
    gateIncarnation = gateNewIncarnation();
}

static void gateInfo(RedisModuleInfoCtx *ctx, int for_crash_report) {
    REDISMODULE_NOT_USED(for_crash_report);
    RedisModule_InfoAddSection(ctx, "");
    RedisModule_InfoAddFieldULongLong(ctx, "apply_mismatches", gateApplyMismatches);
}

/* ------------------------------ Configuration ---------------------------- */

static long long gateConfigGet(const char *name, void *privdata) {
    REDISMODULE_NOT_USED(name);
    return *(long long *)privdata;
}

static int gateConfigSet(const char *name, long long val, void *privdata, RedisModuleString **err) {
    REDISMODULE_NOT_USED(name);
    REDISMODULE_NOT_USED(err);
    *(long long *)privdata = val;
    if (privdata == &gateCfgTestSeed) gateTestState = (uint64_t)val;
    return REDISMODULE_OK;
}

/* --------------------------------- Loading ------------------------------- */

static int gateCommand(RedisModuleCtx *ctx, const char *name, RedisModuleCmdFunc fn,
                       const char *flags, const char *acl, int arity, uint64_t keyflags)
{
    if (RedisModule_CreateCommand(ctx, name, fn, flags, 1, 1, 1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    RedisModuleCommand *cmd = RedisModule_GetCommand(ctx, name);
    if (cmd == NULL) return REDISMODULE_ERR;

    RedisModuleCommandInfo info = {
        .version = REDISMODULE_COMMAND_INFO_VERSION,
        .arity = arity,
        .key_specs = (RedisModuleCommandKeySpec[]){
            {
                .flags = keyflags,
                .begin_search_type = REDISMODULE_KSPEC_BS_INDEX,
                .bs.index.pos = 1,
                .find_keys_type = REDISMODULE_KSPEC_FK_RANGE,
                .fk.range = {0, 1, 0},
            },
            {0}
        },
    };
    if (RedisModule_SetCommandInfo(cmd, &info) == REDISMODULE_ERR) return REDISMODULE_ERR;
    /* Module commands get no ACL category from their flags. */
    if (acl && RedisModule_SetCommandACLCategories(cmd, acl) == REDISMODULE_ERR)
        return REDISMODULE_ERR;
    return REDISMODULE_OK;
}

int RedisModule_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    REDISMODULE_NOT_USED(argv);
    REDISMODULE_NOT_USED(argc);

    if (RedisModule_Init(ctx, "gate", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    gateSetAllocator(RedisModule_Alloc, RedisModule_Calloc, RedisModule_Realloc, RedisModule_Free);
    /* Keys are signalled as modified by hand (gateChanged): a command that
     * is refused or finds nothing to do must not abort a WATCH. */
    RedisModule_SetModuleOptions(ctx, REDISMODULE_OPTIONS_HANDLE_IO_ERRORS |
                                      REDISMODULE_OPTIONS_HANDLE_REPL_ASYNC_LOAD |
                                      REDISMODULE_OPTION_NO_IMPLICIT_SIGNAL_MODIFIED);

    if (RedisModule_RegisterNumericConfig(ctx, "snapshot-examples",
            GATE_DEFAULT_SNAPSHOT_EXAMPLES, REDISMODULE_CONFIG_DEFAULT, 1,
            GATE_MAX_SNAPSHOT_EXAMPLES,
            gateConfigGet, gateConfigSet, NULL, &gateCfgSnapshotExamples) == REDISMODULE_ERR ||
        RedisModule_RegisterNumericConfig(ctx, "feedback-timeout",
            GATE_DEFAULT_FEEDBACK_TIMEOUT_MS, REDISMODULE_CONFIG_DEFAULT, 1,
            GATE_MAX_FEEDBACK_TIMEOUT_MS, gateConfigGet, gateConfigSet, NULL,
            &gateCfgFeedbackTimeout) == REDISMODULE_ERR ||
        /* Testing only: a non-zero seed makes draws and ids predictable.
         * Hidden from CONFIG GET patterns. */
        RedisModule_RegisterNumericConfig(ctx, "test-seed", 0, REDISMODULE_CONFIG_HIDDEN, 0,
            LLONG_MAX, gateConfigGet, gateConfigSet, NULL, &gateCfgTestSeed) == REDISMODULE_ERR ||
        RedisModule_LoadConfigs(ctx) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    RedisModuleTypeMethods tm = {
        .version = REDISMODULE_TYPE_METHOD_VERSION,
        .rdb_load = GateRdbLoad,
        .rdb_save = GateRdbSave,
        .aof_rewrite = GateAofRewrite,
        .mem_usage = GateMemUsage,
        .digest = GateDigest,
        .free = GateFree,
        .free_effort = GateFreeEffort,
        /* No copy callback: COPY is refused, proof must not be duplicated. */
    };
    GateType = RedisModule_CreateDataType(ctx, "gate-type", GATE_CODEC_VERSION, &tm);
    if (GateType == NULL) return REDISMODULE_ERR;

    if (RedisModule_AddACLCategory(ctx, "gate") == REDISMODULE_ERR) return REDISMODULE_ERR;

    const uint64_t rw = REDISMODULE_CMD_KEY_RW | REDISMODULE_CMD_KEY_ACCESS | REDISMODULE_CMD_KEY_UPDATE;
    const uint64_t create = REDISMODULE_CMD_KEY_RW | REDISMODULE_CMD_KEY_INSERT;
    const uint64_t ro = REDISMODULE_CMD_KEY_RO | REDISMODULE_CMD_KEY_ACCESS;

    if (gateCommand(ctx, "GCREATE", GCreate_RedisCommand, "write deny-oom", "write gate", -5, create) == REDISMODULE_ERR ||
        gateCommand(ctx, "GPREDICT", GPredict_RedisCommand, "write deny-oom", "write gate", -4, rw) == REDISMODULE_ERR ||
        gateCommand(ctx, "GLEARN", GLearn_RedisCommand, "write", "write gate", 5, rw) == REDISMODULE_ERR ||
        gateCommand(ctx, "GADD", GAdd_RedisCommand, "write deny-oom", "write gate", -6, rw) == REDISMODULE_ERR ||
        gateCommand(ctx, "GINFO", GInfo_RedisCommand, "readonly", "read gate", 2, ro) == REDISMODULE_ERR ||
        gateCommand(ctx, "GPAUSE", GPause_RedisCommand, "write", "write gate", 2, rw) == REDISMODULE_ERR ||
        gateCommand(ctx, "GRESUME", GResume_RedisCommand, "write", "write gate", 2, rw) == REDISMODULE_ERR ||
        gateCommand(ctx, "GRESET", GReset_RedisCommand, "write", "write gate", 2, rw) == REDISMODULE_ERR ||
        gateCommand(ctx, "_GATE.APPLY", GApply_RedisCommand, "write internal", NULL, -1, create) == REDISMODULE_ERR)
        return REDISMODULE_ERR;

    gateIncarnation = gateNewIncarnation();
    RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_ReplicationRoleChanged, gateRoleChanged);
    RedisModule_RegisterInfoFunc(ctx, gateInfo);
    return REDISMODULE_OK;
}
