# Redis Gate — specification

Version 0.3 (simplified). Supersedes the 0.2 RFC. Status: implemented as a
loadable module, experimental; see section 7.

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

Option words (`ALPHA`, `TEXT`, `LABEL`, ...) are not case-sensitive; labels
and feature names are. Numbers are written in decimal, with or without an
exponent.

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
once. Labels are 1 to 64 bytes of UTF-8 without control characters, each
listed once; a label is in `ALWAYSDEFER` or in `CLASSALPHA`, not both, and
`ALWAYSDEFER` cannot list every label.

Reply: `OK`.

### GPREDICT

Asks the gate about one request.

* Classifier gate: `TEXT` is the message (at most 1 MiB; the first 4,096
  bytes are read). `FEATURES` optionally adds up to 64 named numbers that
  matter for the decision but are not in the text, for example
  `FEATURES 2 mentioned 1 thread_depth 0.3`. Names follow the rule for
  labels and may not repeat. Scale the values to about `[-1, 1]`; anything
  beyond 4 counts as 4.
* Score gate: `SCORE` is your confidence in `[0, 1]`; higher means more
  trustworthy. (A rounding error outside the range, such as 1.0000001, is
  taken as the end of the range.)

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
| `audit-samples`, `audit-errors` | answered spot checks of the answering snapshot, and how many disagreed |
| `calibration-samples` | proof collected by the snapshot being tested (or answering) |
| `pending` | requests waiting for `GLEARN` |
| `missing` | requests that were needed as proof and never got their `GLEARN` |
| `revocations` | times the gate stopped answering by itself |
| `memory` | bytes |

Rates, thresholds and `coverage` are doubles, counters are integers.
`coverage` and `audit-errors / audit-samples` are measurements, not
guarantees. A gate does its bookkeeping when it is written to, so `GINFO`
shows an idle gate as of its last write.

States: `COLD` nothing is being tested (a classifier that has not learned
enough yet, or a gate whose answers stopped arriving); `CALIBRATING` a
snapshot is being tested and nothing answers; `ACTIVE` the gate answers;
`PAUSED` see below. A paused gate still shows what it has proven.

### GPAUSE, GRESUME, GRESET

`GPAUSE` stops the gate from answering: where it would have replied
`ACCEPT` or `AUDIT` it replies `ABSTAIN`. Everything else keeps running, so
a paused gate keeps learning and proving itself (shadow mode). `GRESUME`
lets it answer again. `GRESET` forgets everything learned and proven and
restarts the counters, but keeps the configuration (and a pause); use it
when the teacher or the meaning of the inputs changes. All three reply `OK`.

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
8,192 buckets. There is no vocabulary to maintain.

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
"answer everything" and 0.999, each with a betting test of its own (start
with 1, bet against "the error rate above this threshold is worse than the
target"; grow enough and the threshold is proven). The test may be checked
after every answer without becoming unreliable. Proof always uses the guess
recorded when the request was asked, never a recomputed one, and is counted
in request order, so a slow teacher cannot bias it. The gate answers at the
lowest proven threshold of each label, and keeps testing lower ones, so
coverage grows over time.

**Late and lost answers.** When newer answers are waiting behind a late
one, proof waits for it for a quarter of the feedback timeout (15 seconds by
default), then moves on and counts the request as a wrong guess until its
answer arrives. An answer that has not arrived after the feedback timeout
(one minute) is lost: the request stays a wrong guess and its id is
forgotten. That is the cautious reading. While a threshold is being proven,
a lost answer costs proof (about `2.5 / ALPHA` more requests each), and
answers lost as often as `ALPHA` allows errors stop proof altogether. If
your teacher often needs more than a few seconds, raise
`gate.feedback-timeout`.

**No answers at all.** When 32 requests in a row end without an answer and
no answer of any request has arrived since the first of them was asked, the
feedback path is broken and the gate cannot check itself. It stops
answering, keeps its model, and proves itself again as soon as answers
arrive.

**Replacing the answering snapshot.** A newer snapshot takes over only when
it has proven thresholds of its own and answers more of the same
spot-checked requests than the current one. A classifier snapshot that has
not taken over after a generous amount of proof is dropped and the next one
is tried.

**Getting worse.** Spot checks feed a drift detector tuned to the error
target. Its verdict does not depend on the order or the timing of the
answers. When it fires the gate stops answering at once, forgets its model,
and starts over by itself. A spot check whose answer is lost may hide a
disagreement: when so many are lost that the gate can no longer rule that
out, it stops answering, keeps its model, and proves itself again.

**Budget.** Snapshot number g may use `DELTA / (g (g + 1))` of the lifetime
budget, split evenly over the thresholds it tests. These shares add up to at
most `DELTA` no matter how many snapshots there are, and `GRESET` never
hands any back.

**Full.** A gate holds at most 4,096 requests waiting for feedback. While
full it answers `ABSTAIN` (without an id) to everything, so a slow or broken
feedback path cannot make it trust anything unchecked.

**Redis.** A gate is one key. `GPREDICT` runs on the primary. Replicas and
the AOF receive what actually happened (the random draw, the clock, the
recorded guess), never the command, so they never re-roll dice. An `ACCEPT`
is not replicated and not written to the AOF; the request counters and
`coverage` are therefore kept by the primary only. After a restart, a
failover, a `RESTORE` or any other reload Redis cannot show that nothing
was lost, so the gate keeps its model, drops its proof and its waiting
requests at its first write, and proves itself again by itself.

## 4. The guarantee

For one snapshot and one label, let the *risk* of a threshold be the share
of requests at or above it on which the snapshot disagrees with the teacher.
With probability at least `1 - DELTA` over the life of the key, the gate
never answers at a threshold whose risk is above that label's error rate.

This holds if requests are independent and alike and the teacher's labels
are correct. It does not need every answer to arrive: an answer the gate is
not told counts against it, never for it.

It does not hold for traffic unlike what the gate has seen, for one user or
one request, or across a restart or failover that lost writes. The drift
detector is an alarm, not a proof, and it only sees the spot checks that
were answered. Bursts of near-identical requests and retries weaken the
independence the proof relies on.

## 5. Fixed choices

| Choice | Value |
|---|---|
| buckets | 8,192 |
| thresholds per label | 32, evenly spaced in `log(1 - threshold)`; classifier: from `max(1/classes, 0.25)` to 0.999; score: from 0 to 0.998 |
| betting test | wealth starts at 1 and is multiplied by `1 + c alpha / (1 - alpha)` for a right guess and by `1 - c` for a wrong one; the average over c = 0.1, 0.25, 0.5, 0.9 is what counts |
| proven | when that wealth reaches `32 x labels x g (g + 1) / DELTA` for snapshot g (labels: those the gate may answer; 1 for a score gate) |
| learning rate | 0.1 |
| first snapshot / next snapshot | 1,000 examples (`gate.snapshot-examples`) |
| snapshot dropped | after 10 times the proof a flawless threshold needs, per label, without taking over (classifier); requests it left to the teacher although their label is one the gate may answer count like proof |
| feedback timeout | 1 minute (`gate.feedback-timeout`, milliseconds); proof moves on without a late answer after a quarter of it, or as soon as 16,384 newer answers wait |
| waiting requests | 4,096 (3,072 while they are only wanted as training examples; 16 MiB of stored clues) |
| answers missing in a row before the gate stops | 32 |
| drift detector | likelihood ratios for 2x and 4x the error rate (at most halfway to 1), averaged, over blocks of 64, 512 and 4,096 spot checks in the order they were asked; a spot check counts as right until its answer says otherwise; alarm at 1,000, and at 10^9 with lost answers counted as disagreements |

The two names in parentheses are server configuration values, read when a
gate is created.

Where this differs from the reference design. The reference walks the
thresholds in a fixed order from the most confident one down; here each is
tested on its own, which reaches a first proven threshold sooner (far
sooner for a model that is rarely very sure) and ends with one to three
points less coverage. The reference uses a running likelihood-ratio
detector (e-CUSUM), which is sound when answers arrive in order; here the
detector works on blocks of spot checks in the order they were asked, so
that answers arriving late or in batches cannot set it off. Lost answers
are not part of the reference design.

## 6. Left out on purpose

Compared with the 0.2 RFC: embedding vectors as input, per-gate dimensions,
learning-rate and regularization settings, soft labels, manual freezing,
`GCONFIG`, reset scopes, teacher and schema version strings, certificate
lifetimes, per-gate memory limits, export and import. Each can be added
later without changing the commands above.

## 7. Status

STATUS_PLACEHOLDER
