# Gate: a Redis data type that learns when a cheap answer is good enough

A **gate** sits in front of an expensive decision procedure (the *teacher*:
an LLM call, a router, an answer-equivalence evaluator) and answers one
question per request: *may a cheap prediction replace the teacher this time?*

* `ACCEPT` - use the cheap prediction, skip the teacher.
* `ABSTAIN` - call the teacher as usual and tell the gate what it said.
* `AUDIT` - the gate would have accepted, but this request was randomly
  selected to keep the gate honest: call the teacher and report back.

You give the gate an error budget (`ALPHA`: at most this fraction of accepted
requests may disagree with the teacher). The gate only starts accepting once
audited evidence supports that budget, and it keeps auditing afterwards.

A gate measures **agreement with the designated teacher on the traffic it was
calibrated on**. It does not measure truth, and it promises nothing about any
individual request. See [What the guarantee is](#what-the-guarantee-is).

> Status: experimental reference implementation of the GATE specification
> (`SPEC.md`). Not production ready: see [Limitations](#limitations).

## Contents

* [Quick start](#quick-start)
* [Concepts](#concepts)
* [Command reference](#command-reference)
* [Client integration](#client-integration)
* [Persistence, replication and recovery](#persistence-replication-and-recovery)
* [Limits and defaults](#limits-and-defaults)
* [What the guarantee is](#what-the-guarantee-is)
* [Limitations](#limitations)
* [Building and testing](#building-and-testing)

## Quick start

```sh
make -C modules/gate                      # builds modules/gate/gate.so
./src/redis-server --loadmodule ./modules/gate/gate.so
```

A gate over an external score (for example a semantic-cache similarity):

```
> GCREATE cache:gate MODE SCORE ALPHA 0.05 TEACHER equivalence-v1 SCHEMAVERSION cosine-v1
OK
> GFREEZE cache:gate                      # start calibrating generation 1
(integer) 1
> GPREDICT cache:gate SCORE 0.93
1# "action" => "ABSTAIN"
2# "reason" => "COLD_START"
3# "observation_id" => "5d0c3f9a7b21e684-1"
> GLEARN cache:gate OBSERVATION 5d0c3f9a7b21e684-1 CORRECT 1
APPLIED
  ... more traffic ...
> GPREDICT cache:gate SCORE 0.95
1# "action" => "ACCEPT"
2# "score" => (double) 0.95
3# "generation" => (integer) 1
4# "risk_target" => (double) 0.05
```

A gate that learns its own classifier inside Redis:

```
> GCREATE agent:respond MODE CLASSIFIER CLASSES 2 ignore respond INPUTS 2 TEXT FEATURES ALPHA 0.02 TEACHER router-v1 SCHEMAVERSION chat-v1
OK
> GADD agent:respond TEXT "Please explain this" FEATURES 2 mentioned 1 previous_speaker_agent 0 LABEL respond
(integer) 1
> GADD agent:respond TEXT "Thanks!" FEATURES 2 mentioned 0 previous_speaker_agent 0 LABEL ignore
(integer) 2
> GFREEZE agent:respond
(integer) 1
> GPREDICT agent:respond TEXT "thank you so much" FEATURES 2 mentioned 0 previous_speaker_agent 0
1# "action" => "ABSTAIN"
2# "reason" => "COLD_START"
3# "observation_id" => "0b7e41c2a9d3f586-1"
> GLEARN agent:respond OBSERVATION 0b7e41c2a9d3f586-1 LABEL ignore
APPLIED
```

## Concepts

**Teacher.** The existing, trusted decision procedure. It lives in your
application; Redis never calls it. `TEACHER` names its version.

**Modes.**

* `SCORE` - your application already has a model. It passes a confidence
  score in `[0,1]` (higher = more trustworthy) and the gate learns which score
  threshold is safe. Feedback is `CORRECT 0|1`: did the cheap result agree
  with the teacher?
* `CLASSIFIER` - the gate trains a small linear classifier inside Redis from
  the teacher's labels (hashed text, a client-supplied embedding, named
  numeric features) and gates on its own confidence. Feedback is the
  teacher's `LABEL` (or class `PROBS`).

**Generation.** `GFREEZE` snapshots the current model into an immutable
*generation* (1, 2, 3, ... per key, never reused). A new generation is the
**candidate**: it is tested, in the background, on audited traffic. When the
evidence certifies one of its acceptance regions it is promoted and becomes
the **serving** generation. Training never touches a frozen generation; to
serve a better model you freeze a new generation.

**Acceptance region.** "score >= threshold" (optionally per predicted class).
The gate tests a fixed grid of `THRESHOLDS` and serves the lowest threshold
that has been certified.

**Audit bit.** Every `GPREDICT` flips an independent coin *before* deciding
anything. Requests whose coin comes up are **eligible evidence**: their
teacher label must be delivered with `GLEARN`. The coin's probability is
`AUDIT` while a policy is serving and `COLDAUDIT` otherwise.

**Observation.** A request the gate wants feedback for. `GPREDICT` returns an
opaque `observation_id`; `GLEARN` consumes it exactly once. If a reply has no
`observation_id`, no feedback is wanted.

**States.**

| State | Meaning | `GPREDICT` |
|---|---|---|
| `COLD` | no generation yet | always `ABSTAIN` |
| `CALIBRATING` | a candidate is being tested, nothing is serving | always `ABSTAIN` |
| `ACTIVE` | a certified generation is serving | `ACCEPT` / `AUDIT` / `ABSTAIN` |
| `REVOKED` | certification was withdrawn, or recovery is required | always `ABSTAIN` |
| `PAUSED` | acceptance administratively disabled (shadow mode) | always `ABSTAIN` |

`ACTIVE` can coexist with a background candidate. `PAUSED` hides the state
underneath: calibration, auditing and training all continue, only `ACCEPT` is
suppressed.

## Command reference

All commands take exactly one key, run on the primary, and are atomic. Option
names are case-insensitive. Unless noted, options may appear in any order and
at most once. Arguments are fully validated before anything is changed.

Common errors:

| Error | When |
|---|---|
| `ERR no such key` | the key does not exist (every command except `GCREATE`) |
| `WRONGTYPE Operation against a key holding the wrong kind of value` | the key is not a gate |
| `BUSYKEY Target key name already exists.` | `GCREATE` on an existing key |
| `ERR syntax error ...` / `ERR invalid ...` | malformed or out-of-range arguments |

`TYPE key` returns `gate-type`. `DEL`, `UNLINK`, `EXPIRE`, `RENAME`, `MOVE`,
`MEMORY USAGE`, `OBJECT`, `DUMP`/`RESTORE` and eviction work as for any key.
`COPY` is refused (a certified gate must not be duplicated).

### GCREATE

```
GCREATE key MODE <SCORE | CLASSIFIER> ALPHA alpha
  [DELTA delta] [AUDIT q] [COLDAUDIT q] [THRESHOLDS n]
  [CLASSES count label [label ...]]
  [CLASSALPHA count label alpha [label alpha ...]]
  [ALWAYSDEFER count label [label ...]]
  [INPUTS count <TEXT | VECTOR | FEATURES> [<TEXT | VECTOR | FEATURES> ...]]
  [TEXTDIM n] [VECTORDIM n] [FEATUREDIM n] [LR rate] [L2 lambda]
  [TEACHER version] [SCHEMAVERSION version]
  [MAXMEMORY bytes] [MAXPENDING n] [OBSERVATIONTTL ms] [CERTTTL ms]
  [DRIFTMARGIN m] [ONMISSING <REVOKE | ERROR>] [EXPAND <NO | YES>]
```

Creates a gate in state `COLD`. Fails if the key exists; never overwrites.

| Option | Default | Meaning |
|---|---|---|
| `MODE` | required | `SCORE` or `CLASSIFIER` |
| `ALPHA alpha` | required | risk target: tolerated disagreement rate among accepted requests, `0 < alpha < 1` |
| `DELTA delta` | `0.01` | lifetime probability of ever certifying a policy whose risk exceeds its target, `0 < delta < 1` |
| `AUDIT q` | `0.05` | audit probability while a policy is serving, `0 < q <= 1` |
| `COLDAUDIT q` | same as `AUDIT` | audit probability while nothing is serving, `0 < q <= 1` |
| `THRESHOLDS n` | `32` | size of the fixed threshold grid, `2..256` |
| `CLASSES count label ...` | none | class labels, in order. Required (>= 2 distinct) for `CLASSIFIER`; optional for `SCORE` |
| `CLASSALPHA count label alpha ...` | none | per-class risk target (must be `<= ALPHA`); makes the gate class-conditional |
| `ALWAYSDEFER count label ...` | none | never accept this predicted class; makes the gate class-conditional |
| `INPUTS count ...` | required for `CLASSIFIER` | the request sections every `GADD`/`GPREDICT` must carry |
| `TEXTDIM n` | `8192` | hash buckets of the text block (needs `TEXT` input) |
| `VECTORDIM n` | required with `VECTOR` | embedding dimension |
| `FEATUREDIM n` | `256` | hash buckets of the named-feature block (needs `FEATURES` input) |
| `LR rate` | `0.1` | SGD learning rate, `0 < rate <= 10` |
| `L2 lambda` | `0` | L2 regularization, `0 <= rate*lambda < 1`; any value above 0 makes every update O(model size) |
| `TEACHER version` | `dev` | version of the teacher that produces the labels |
| `SCHEMAVERSION version` | `dev` | version of the feature / scoring policy |
| `MAXMEMORY bytes` | 64 MiB | per-gate memory limit |
| `MAXPENDING n` | `4096` | maximum outstanding observations |
| `OBSERVATIONTTL ms` | `300000` | how long an observation waits for feedback |
| `CERTTTL ms` | 30 days | lifetime of a certification |
| `DRIFTMARGIN m` | the row's alpha | the drift guard revokes when audited error exceeds `alpha + m` |
| `ONMISSING` | `REVOKE` | what a missing eligible label does, see below |
| `EXPAND` | `NO` | keep testing after promotion and extend the policy, see below |

Rules:

* `SCORE` gates reject `INPUTS`, `TEXTDIM`, `VECTORDIM`, `FEATUREDIM`, `LR`
  and `L2`.
* With no class option the gate tests one pooled set of regions. With
  `CLASSALPHA` and/or `ALWAYSDEFER` it tests one set per predicted class and
  each class is certified separately; classes without a certified region
  defer. Class options require `CLASSES`.
* `TEACHER` and `SCHEMAVERSION` should always be set explicitly in
  production; the `dev` default exists for local experiments.
* Labels, feature names and versions must be valid UTF-8 without control
  characters (labels up to 64 bytes, versions and names up to 128).
* `ONMISSING REVOKE` (specification behaviour): an eligible observation that
  expires without feedback, or cannot be stored, invalidates the evidence it
  belonged to: the candidate is discarded and/or serving is revoked
  (`FEEDBACK_INTEGRITY`). `ONMISSING ERROR`: it is counted as a disagreement
  instead, which is conservative and keeps the gate running.
* `EXPAND NO` (specification behaviour): the policy is fixed when the first
  region certifies. `EXPAND YES`: the serving generation keeps testing its
  remaining regions with the budget they were already allotted and lowers
  its thresholds (or adds classes) as they certify.

Reply: `OK`.

### GADD

```
GADD key [TEXT text] [VECTOR blob] [FEATURES count name value [name value ...]]
     <LABEL label | PROBS p [p ...]>
```

`CLASSIFIER` only. Trains the mutable trainer on one labelled example
immediately. The request must carry exactly the sections configured by
`INPUTS`. `PROBS` takes exactly one probability per class, in `CLASSES`
order; they must be finite, non-negative and sum to 1 within `0.001` (they are
renormalized).

`GADD` is for historical examples and bootstrapping. **It never counts as
calibration or drift evidence**, and it never changes a frozen generation.

`VECTOR` is a blob of `VECTORDIM` little-endian IEEE-754 float32 values.

Reply: integer, the trainer's total number of updates.

### GPREDICT

```
GPREDICT key [TEXT text] [VECTOR blob] [FEATURES count name value [name value ...]]
GPREDICT key SCORE score [LABEL label]
```

The first form is for `CLASSIFIER` gates, the second for `SCORE` gates
(`score` in `[0,1]`; `LABEL` is required exactly when the gate has `CLASSES`).

Evaluates the request against the serving policy, draws the audit bit, and
registers an observation when feedback is wanted.

Reply: a map (a flat field/value array in RESP2). Fields appear in this order
and are omitted when they do not apply.

| `action` | Fields | What the client must do |
|---|---|---|
| `ACCEPT` | `label`, `score`, `generation`, `risk_target` | Use the cheap prediction. Do not call the teacher. No feedback. |
| `AUDIT` | `label`, `score`, `observation_id`, `generation` | Call the teacher, **use the teacher's answer**, send `GLEARN`. |
| `ABSTAIN` | `reason`, `observation_id`, `generation` | Call the teacher, use its answer; send `GLEARN` if there is an `observation_id`. |

* `label` - predicted class (absent for `SCORE` gates without `CLASSES`).
* `score` - confidence of the serving model: the largest class probability
  (`CLASSIFIER`) or the score you passed (`SCORE`). It is a ranking signal,
  not a probability of being correct.
* `generation` - the serving generation; absent when nothing is serving.
* `risk_target` - the alpha the accepting region was certified for.
* `observation_id` - opaque; pass it to `GLEARN` unchanged.

`reason` values:

| Reason | Meaning |
|---|---|
| `COLD_START` | no policy has been certified yet |
| `BELOW_THRESHOLD` | score below the certified threshold |
| `CLASS_DEFERRED` | predicted class is `ALWAYSDEFER` or has no certified region |
| `PAUSED` | the gate is paused |
| `DRIFT_DETECTED` | serving was revoked by the drift guard |
| `CERT_EXPIRED` | the certification outlived `CERTTTL` |
| `FEEDBACK_INTEGRITY` | serving was revoked because audit feedback went missing |
| `TEACHER_CHANGED` / `SCHEMA_CHANGED` / `CONFIG_CHANGED` | serving was revoked by `GCONFIG` |
| `RECOVERY_REQUIRED` | the gate was reloaded or failed over and awaits `GRECOVER` |
| `RESOURCE_LIMIT` | the observation could not be recorded (`MAXPENDING` / `MAXMEMORY`); never has an `observation_id` |

A retried `GPREDICT` is a new request and a new observation.

### GLEARN

```
GLEARN key OBSERVATION id <LABEL label | PROBS p [p ...] | CORRECT <0 | 1>>
       [TEACHER version]
```

Delivers the teacher's verdict for an observation.

* `LABEL` / `PROBS` - `CLASSIFIER` gates. With `PROBS` the trainer learns
  from the soft targets and the class with the largest probability (first one
  on ties) is the reference label for evidence.
* `CORRECT 1` / `CORRECT 0` - `SCORE` gates: the cheap candidate result did /
  did not agree with the teacher.
* `TEACHER version` - the teacher that produced this verdict. Omitting it
  attests that it was the gate's configured teacher.

Evidence is always computed from the predictions frozen at `GPREDICT` time,
never recomputed with newer weights, and applied in observation order.

Reply: a status (simple string). Only `APPLIED` changes the gate.

| Status | Meaning |
|---|---|
| `APPLIED` | consumed: trained on it and/or counted it as evidence |
| `DUPLICATE` | this observation was already consumed |
| `EXPIRED` | feedback arrived after `OBSERVATIONTTL` |
| `INVALIDATED` | the observation was dropped by a reset or a teacher/schema change |
| `UNKNOWN` | not an id issued by this gate's current lifecycle |
| `TEACHER_MISMATCH` | `TEACHER` differs from the gate's teacher; the observation stays pending |

Retrying `GLEARN` is safe: an id is consumed at most once.

### GFREEZE

```
GFREEZE key
```

Freezes the trainer (`CLASSIFIER`) or opens a calibration round for the
external scorer (`SCORE`) as a new candidate generation, and spends that
generation's share of `DELTA`. Only requests predicted **after** the freeze
are evidence for it.

Errors: a candidate is already under test (cancel it with
`GRESET key CANDIDATE`); a `CLASSIFIER` trainer with no training updates; the
gate requires recovery.

Reply: integer, the new generation number.

### GINFO

```
GINFO key [FULL]
```

Read-only; allowed on replicas. Reply: a map (flat array in RESP2).

| Field | Type | Meaning |
|---|---|---|
| `mode` | string | `SCORE` / `CLASSIFIER` |
| `state` | string | `COLD` / `CALIBRATING` / `ACTIVE` / `REVOKED` / `PAUSED` |
| `reason` | string | why nothing is being accepted right now (`NONE` when serving) |
| `paused`, `recovery_required` | integer | flags |
| `teacher`, `schema_version` | string | configured versions |
| `lineage`, `epoch` | string | ids of the budget ledger and of the current observation-id epoch |
| `created_at` | integer | unix ms |
| `config` | map | every `GCREATE` option with its effective value |
| `budget` | map | `delta_total`, `delta_spent`, `delta_remaining`, `generations`, `recoveries` |
| `trainer` | map or null | `updates`, `rejected`, `dimension` (`CLASSIFIER` only) |
| `candidate` | map or null | the generation under test |
| `serving` | map or null | the serving generation |
| `requests` | map | `total`, `accepted`, `audited`, `abstained`, `would_accept`, `coverage`, `acceptance_rate`, `abstain_reasons` |
| `feedback` | map | `observations`, `eligible`, `applied`, `duplicate`, `expired`, `invalidated`, `unknown`, `teacher_mismatch`, `pending`, `pending_eligible`, `oldest_pending_age`, `observations_expired`, `eligible_expired`, `imputed`, `resource_limited` |
| `events` | map | `promotions`, `revocations`, `candidates_discarded` |
| `last_revocation` | map or null | `reason`, `at`, `generation` |
| `memory` | map | `bytes`, `limit`, `pending_bytes` |

A generation map (`candidate`, `serving`) contains:

| Field | Meaning |
|---|---|
| `generation` | generation number |
| `frozen_at`, `promoted_at`, `expires_at` | unix ms (`promoted_at`/`expires_at` only for serving) |
| `delta`, `delta_region`, `regions` | this generation's budget, the share of each region, and the number of regions tested |
| `log_wealth_required` | evidence a region needs to certify, `-ln(delta_region)` |
| `samples`, `errors`, `imputed` | eligible samples applied, disagreements among them, and how many were imputed |
| `testing` | `1` while regions are still being tested |
| `coverage` | estimated share of traffic inside the current policy |
| `policy` | array with one map per class (or one pooled `*` entry): `class`, `risk_target`, `status` (`CERTIFIED` / `TESTING` / `DEFERRED`), `threshold` (null until certified), `coverage`, `best_log_wealth`, and for serving `audits`, `audit_errors`, `audit_risk`, `window`, `window_errors` |
| `evidence` | `FULL` only: one map per tested region: `class`, `threshold`, `samples`, `errors`, `log_wealth`, `certified` |

In `requests`, `coverage` is the fraction of requests that fell inside the
serving policy and `acceptance_rate` is the fraction that were actually
accepted, i.e. teacher calls avoided. `audit_risk` and every `coverage` are
empirical estimates, not certified bounds.

### GRESET

```
GRESET key <CANDIDATE | CALIBRATION | MODEL | ALL>
```

| Scope | Effect |
|---|---|
| `CANDIDATE` | cancel the candidate under test; serving is untouched |
| `CALIBRATION` | discard candidate and serving certification; keep the trainer. The gate becomes `COLD` |
| `MODEL` | as `CALIBRATION`, and zero the trainer weights |
| `ALL` | as `MODEL`, and drop pending observations and operational counters |

No reset ever gives budget back: the generation counter and the lineage are
preserved, so a cancelled generation's share of `DELTA` stays spent.

Reply: `OK`.

### GPAUSE / GRESUME

```
GPAUSE key
GRESUME key
```

`GPAUSE` stops the gate from accepting; everything else keeps running
(shadow mode). `GRESUME` lifts the pause; the gate accepts again only if a
valid serving generation still exists. Both are idempotent.

Reply: `OK`.

### GCONFIG

```
GCONFIG key SET <AUDIT q | TEACHER version | SCHEMAVERSION version> [...]
```

Changes one or more parameters atomically. Setting a parameter to its current
value does nothing. A real change invalidates what depended on it:

| Parameter | Invalidates |
|---|---|
| `AUDIT` | candidate and serving certification (`CONFIG_CHANGED`) |
| `TEACHER` | candidate, serving (`TEACHER_CHANGED`), trainer weights and all pending observations |
| `SCHEMAVERSION` | candidate, serving (`SCHEMA_CHANGED`), trainer weights and all pending observations |

Spent budget is never restored.

Reply: `OK`.

### GRECOVER

```
GRECOVER key
```

Acknowledges a recovery. After a restart, a failover or a `RESTORE`, Redis
cannot prove that the gate's ledger and observations are complete, so the
gate fails closed: certification and pending observations are dropped and
`GPREDICT` abstains with `RECOVERY_REQUIRED`. `GRECOVER` declares a new risk
lifecycle: the gate becomes `COLD`, keeps its trainer and its (never
replenished) budget ledger, and can be frozen again.

Error: the gate is not awaiting recovery.

Reply: `OK`.

## Client integration

```python
def decide(r, teacher, text, context):
    try:
        reply = gpredict(r, "agent:respond", text=text, features=context)
    except Exception:                 # missing key, timeout, Redis down
        return teacher(text, context)
    if reply["action"] == "ACCEPT":
        return reply["label"]
    label = teacher(text, context)    # ABSTAIN and AUDIT both call the teacher
    oid = reply.get("observation_id")
    if oid:
        glearn(r, "agent:respond", oid, label=label)   # safe to retry
    return label
```

`examples/gate_client.py` is a runnable version with RESP2/RESP3 decoding.

For a semantic cache (`SCORE` mode): pass the similarity as `SCORE`. On
`ACCEPT` serve the cached answer. Otherwise call the LLM; if the reply has an
`observation_id`, also judge whether the cached candidate was equivalent to
the fresh answer and send `CORRECT 1|0`. No `observation_id` means the gate
does not need that judgement, so the evaluator call can be skipped.

Suggested ACL split: application servers get `+GPREDICT +GLEARN`; the
training pipeline gets `+GADD`; operators get the rest. Whoever may call
`GLEARN` can teach the gate anything: authenticate the teacher at your
service boundary. All commands are in the `@gate` category.

## Persistence, replication and recovery

* The whole gate (configuration, weights, evidence, ledger, pending
  observations) is saved in RDB and rewritten into the AOF.
* Commands are **not** replicated as issued. The primary draws the audit bit,
  reads the clock and evaluates the frozen models once, then replicates those
  realized values through an internal command, so replicas and AOF reloads
  reach the same state without re-rolling any dice.
* `GPREDICT` is a write command: it is refused on read-only replicas.
* Recovery fails closed. A gate that was loaded from disk, restored from a
  dump, or that lives on a replica that got promoted requires `GRECOVER`
  before it can certify again. The lifetime `DELTA` guarantee does not span
  an untracked rollback; `budget.recoveries` counts acknowledged ones.

## Limits and defaults

| Limit | Value |
|---|---|
| classes | 2..64 |
| thresholds | 2..256 |
| `TEXTDIM` | up to 1048576 |
| `VECTORDIM` | up to 16384 |
| `FEATUREDIM` | up to 65536 |
| `TEXT` argument | up to 65536 bytes |
| named features per request | up to 1024 |
| `MAXPENDING` | up to 1048576 |
| `MAXMEMORY` | up to 256 MiB |

## What the guarantee is

For a frozen generation and one acceptance region `A`, define the selective
risk `R(A) = P(prediction != teacher | request in A)`. With probability at
least `1 - DELTA` over the whole life of the key, no region the gate ever
certifies has `R(A) > alpha`, **provided that** audited requests are an
independent, identically distributed sample of the traffic, teacher labels
are correct, and every eligible label is delivered (or imputed as an error).

It is not a statement about any single request, about traffic that differs
from the calibration traffic, about users or sub-populations, or about truth
as opposed to teacher agreement. The drift guard is a heuristic alarm.

## Limitations

See `SPEC.md`, "Implementation status and deviations".

## Building and testing

```sh
make -C modules/gate                 # gate.so
make -C modules/gate test-unit       # standalone C unit tests
make -C modules/gate sim             # certification Monte Carlo
make -C modules/gate test            # unit + Redis integration tests
```
