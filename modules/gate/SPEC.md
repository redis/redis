# Redis Gate — specification

Version 0.3 (simplified). Supersedes the 0.2 RFC. Status: being implemented;
not production ready.

Reference design: <https://strangechances.com/writings/conformal-gate/>.
Statistical background: Learn then Test, <https://arxiv.org/abs/2110.01052>.

## 1. What a gate is

A gate sits in front of something expensive (the *teacher*: an LLM call, a
router, an answer checker) and decides, per request, whether a cheap guess is
good enough to skip it.

You state the error rate you can live with. The gate watches the teacher's
answers, proves on spot-checked requests that its confident guesses stay
inside that error rate, and only then starts answering by itself. It keeps
spot-checking afterwards and stops by itself if it gets worse.

There are two kinds of gate:

* a **classifier gate** learns to guess the teacher's label from text;
* a **score gate** has no model: your application supplies a confidence
  score and the gate learns which scores are safe.

A gate measures agreement with the teacher on the traffic it has seen. It
does not measure truth and it promises nothing about one particular request.

## 2. The whole API

```
GCREATE key ALPHA alpha CLASSES count label [label ...]
        [AUDIT rate] [DELTA delta]
        [ALWAYSDEFER count label [label ...]]
        [CLASSALPHA count label alpha [label alpha ...]]
GCREATE key ALPHA alpha SCORE [AUDIT rate] [DELTA delta]

GPREDICT key TEXT text [FEATURES count name value [name value ...]]
GPREDICT key SCORE score

GLEARN key id LABEL label
GLEARN key id CORRECT <0 | 1>

GADD key TEXT text [FEATURES count name value [name value ...]] LABEL label

GINFO key
GPAUSE key
GRESUME key
GRESET key
```

That is all. `DEL`, `EXPIRE`, `RENAME`, `TYPE` (returns `gate-type`), `DUMP` /
`RESTORE` and `MEMORY USAGE` work as for any key. `COPY` is refused.

### GCREATE

Creates a gate. Fails with `BUSYKEY` if the key exists.

| Option | Default | Meaning |
|---|---|---|
| `ALPHA alpha` | required | tolerated error rate among the requests the gate answers by itself, `0 < alpha < 1` |
| `CLASSES count label ...` | | classifier gate: the 2 to 64 labels the teacher can answer |
| `SCORE` | | score gate |
| `AUDIT rate` | `0.05` | share of answerable requests still sent to the teacher as spot checks, `0 < rate <= 1` |
| `DELTA delta` | `0.01` | chance, over the whole life of the key, of ever trusting a guess that does not deserve it, `0 < delta < 1` |
| `ALWAYSDEFER count label ...` | none | never answer with these labels; always ask the teacher |
| `CLASSALPHA count label alpha ...` | `ALPHA` | a stricter error rate for some labels (must be `<= ALPHA`) |

Exactly one of `CLASSES` and `SCORE` is required. `ALWAYSDEFER` and
`CLASSALPHA` need `CLASSES`. Options may come in any order, each at most
once. Labels are 1 to 64 bytes of UTF-8 without control characters.

Reply: `OK`.

### GPREDICT

Asks the gate about one request.

* Classifier gate: `TEXT` is the message. `FEATURES` optionally adds named
  numbers that matter for the decision but are not in the text, for example
  `FEATURES 2 mentioned 1 thread_depth 0.3`. Scale them to about `[-1, 1]`.
* Score gate: `SCORE` is your confidence in `[0, 1]`; higher means more
  trustworthy.

Reply: an array of two elements, the action and its argument.

| Reply | Meaning | What the client does |
|---|---|---|
| `ACCEPT` *label* | the gate answers | use the label; do not call the teacher. For a score gate the second element is null: use your cheap result |
| `ABSTAIN` *id* | the gate does not know | call the teacher, use its answer, then `GLEARN` with the id |
| `AUDIT` *id* | the gate would have answered, but this request is a spot check | same as `ABSTAIN` |
| `ABSTAIN` *null* | the gate does not know and wants no feedback | call the teacher, use its answer |

A retried `GPREDICT` is a new request.

### GLEARN

Tells the gate what the teacher said about a request.

* Classifier gate: `LABEL label`, the teacher's label.
* Score gate: `CORRECT 1` if your cheap result agreed with the teacher,
  `CORRECT 0` if it did not.

Reply: integer `1` if the feedback was used, `0` if the id is unknown, too
old or already used. Retrying is always safe.

### GADD

Classifier gates only. Teaches the gate one labelled example directly, for
example from history. It makes the model better; it never counts as proof.

Reply: integer, the number of examples the gate has learned from.

### GINFO

Reply: a map (a flat field/value array in RESP2).

| Field | Meaning |
|---|---|
| `kind` | `classifier` or `score` |
| `state` | `COLD`, `CALIBRATING`, `ACTIVE` or `PAUSED` |
| `alpha`, `delta`, `audit` | configuration |
| `classes` | the labels (empty for a score gate) |
| `alphas` | error rate per label; `0` means always defer (one element for a score gate) |
| `thresholds` | lowest score the gate currently answers at, per label; null where it does not answer yet (one element for a score gate) |
| `generation` | number of the snapshot now answering, `0` if none |
| `candidate` | number of the snapshot being tested, `0` if none |
| `learned` | labelled examples the model has learned from |
| `requests`, `accepted`, `audited`, `abstained` | request counters |
| `coverage` | estimated share of requests the gate can answer now |
| `audit-samples`, `audit-errors` | spot checks of the answering snapshot, and how many disagreed |
| `calibration-samples` | proof collected by the snapshot being tested (or answering) |
| `pending` | requests waiting for `GLEARN` |
| `missing` | requests that needed `GLEARN` and never got it |
| `revocations` | times the gate stopped answering by itself |
| `memory` | bytes |

`coverage` and `audit-errors / audit-samples` are measurements, not
guarantees.

States: `COLD` nothing is being tested yet; `CALIBRATING` a snapshot is
being tested and nothing answers; `ACTIVE` the gate answers; `PAUSED` see
below.

### GPAUSE, GRESUME, GRESET

`GPAUSE` stops the gate from answering. Everything else keeps running, so a
paused gate keeps learning and proving itself (shadow mode). `GRESUME` lets
it answer again. `GRESET` forgets everything learned and proven but keeps
the configuration; use it when the teacher or the meaning of the inputs
changes. All three reply `OK`.

### Errors

`ERR no such key` (every command but `GCREATE` on a missing key), `WRONGTYPE`,
`BUSYKEY`, and `ERR ...` for bad arguments. Arguments are checked completely
before anything changes.

### Client loop

```python
action, arg = r.execute_command("GPREDICT", key, "TEXT", text)
if action == b"ACCEPT":
    return arg
label = teacher(text)                       # ABSTAIN and AUDIT
if arg is not None:
    r.execute_command("GLEARN", key, arg, "LABEL", label)
return label
```

If Redis is unreachable or returns an error, call the teacher.

## 3. What happens under the covers

Nothing in this section needs configuring.

**Clues.** The text is cut into words, pairs of adjacent words, four-letter
chunks of longer words, and a few structural hints (length, `?` `!` `@`,
links, code blocks). Each clue, and each named feature, is hashed into one of
8,192 buckets. There is no vocabulary to maintain. Only the first 4,096 bytes
of a text are read.

**Model.** One learned weight per bucket and label. A guess is the label
whose weights add up highest; its confidence is the model's probability for
that label. The model learns a little from every label it is given
(`GLEARN` and `GADD`).

**Snapshots.** Proof is only valid for a model that does not change, so the
gate tests frozen copies. A classifier gate takes its first snapshot after
1,000 labelled examples and a fresh one every 1,000 after that while none is
being tested. A score gate has nothing to freeze; its first test starts at
`GCREATE`. Snapshots are numbered 1, 2, 3, ... and a number is never reused.

**Spot checks.** Every `GPREDICT` draws a random number before anything is
decided. A request the gate could answer is sent to the teacher anyway with
probability `AUDIT`: that is an `AUDIT` reply. While a label has no proven
threshold yet, every one of its requests goes to the teacher and every
teacher answer counts as proof; nothing is wasted while warming up.

**Proof.** For each label the gate tries 32 confidence thresholds between
"answer everything" and 0.999. For each it runs a betting test (start with
1, bet against "the error rate above this threshold is worse than the
target"; grow enough and the threshold is proven). The test may be checked
after every answer without becoming unreliable. Proof always uses the guess
recorded when the request was asked, never a recomputed one, and is applied
in request order, so a slow teacher cannot bias it. The gate answers at the
lowest proven threshold of each label, and keeps testing lower ones, so
coverage grows over time.

**A request that never gets its `GLEARN`** within 5 minutes counts as a
wrong guess if it was needed as proof. That is the cautious reading, and it
means lost feedback costs a little coverage instead of shutting the gate.

**Replacing the answering snapshot.** A newer snapshot takes over only when
it has proven thresholds of its own and answers more of the same requests
than the current one. A classifier snapshot that has not taken over after a
generous amount of proof is dropped and the next one is tried.

**Getting worse.** Spot-check results feed a drift detector tuned to the
error target. When it fires the gate stops answering at once, forgets its
model, and starts over by itself.

**Budget.** Snapshot number g may use `DELTA / (g (g + 1))` of the lifetime
budget, split over what it tests. These shares add up to at most `DELTA` no
matter how many snapshots there are, and `GRESET` never hands any back.

**Full.** A gate holds at most 4,096 requests waiting for feedback. While
full it answers `ABSTAIN` to everything, so a slow or broken feedback path
cannot make it trust anything unchecked.

**Redis.** A gate is one key. `GPREDICT` runs on the primary. Replicas and
the AOF receive what actually happened (the random draw, the clock, the
recorded guess), never the command, so they never re-roll dice. An `ACCEPT`
writes nothing. After a restart or a failover Redis cannot show that nothing
was lost, so the gate keeps its model, drops its proof and its waiting
requests, and proves itself again by itself.

## 4. The guarantee

For one snapshot and one label, let the *risk* of a threshold be the share
of requests at or above it on which the snapshot disagrees with the teacher.
With probability at least `1 - DELTA` over the life of the key, the gate
never answers at a threshold whose risk is above that label's error rate.

This holds if requests are independent and alike, the teacher's labels are
correct, and the gate is told the teacher's answer whenever it asks.

It does not hold for traffic unlike what the gate has seen, for one user or
one request, or across a restart or failover that lost writes. The drift
detector is an alarm, not a proof. Bursts of near-identical requests and
retries weaken the independence the proof relies on.

## 5. Fixed choices

| Choice | Value |
|---|---|
| buckets | 8,192 |
| thresholds per label | 32, evenly spaced in `log(1 - threshold)`; classifier: from `max(1/classes, 0.25)` to 0.999; score: from 0 to 0.998 |
| betting fractions | 0.1, 0.25, 0.5, 0.9 of the maximum, equal weights |
| threshold order (classifier) | from the most confident down, with entry points at every eighth threshold |
| threshold order (score) | every threshold tested on its own |
| learning rate | 0.1 |
| first snapshot / next snapshot | 1,000 examples (`gate.snapshot-examples`) |
| feedback window | 5 minutes (`gate.feedback-timeout`) |
| waiting requests | 4,096 |
| drift detector | likelihood ratios for 2x and 4x the error rate, alarm at 1,000 |

The two names in parentheses are server configuration values, read when a
gate is created.

## 6. Left out on purpose

Compared with the 0.2 RFC: embedding vectors as input, per-gate dimensions,
learning-rate and regularization settings, soft labels, manual freezing,
`GCONFIG`, reset scopes, teacher and schema version strings, certificate
lifetimes, per-gate memory limits, export and import. Each can be added
later without changing the commands above.
