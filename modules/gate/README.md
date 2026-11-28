# Gate

A Redis data type that learns when a cheap answer is good enough.

A gate sits in front of something expensive (an LLM call, a router, an answer
checker: the *teacher*) and decides, per request, whether a cheap guess may
replace it. You say what error rate you can live with; the gate proves on
spot-checked requests that it stays inside it, then starts answering by
itself, and keeps checking afterwards.

`SPEC.md` is the specification: every command, what happens under the
covers, and exactly what is and is not guaranteed. This file is the short
version.

> Experimental. Not production ready; see [Status](#status).

## Build and load

```sh
make -C modules/gate
./src/redis-server --loadmodule ./modules/gate/gate.so
```

## Use

A gate that learns to guess a label from text:

```
> GCREATE agent:respond ALPHA 0.05 CLASSES 2 ignore respond
OK
> GPREDICT agent:respond TEXT "thanks everyone!"
1) "ABSTAIN"
2) "5d0c3f9a7b21e684-1"
> GLEARN agent:respond 5d0c3f9a7b21e684-1 LABEL ignore
(integer) 1
```

At first it always abstains: the application calls the teacher and reports
the answer with `GLEARN`. After enough of that:

```
> GPREDICT agent:respond TEXT "thanks team :)"
1) "ACCEPT"
2) "ignore"
```

A gate over a score your application already has (say, how similar a cached
answer is to the question):

```
> GCREATE cache:gate ALPHA 0.05 SCORE
OK
> GPREDICT cache:gate SCORE 0.93
1) "ABSTAIN"
2) "0b7e41c2a9d3f586-1"
> GLEARN cache:gate 0b7e41c2a9d3f586-1 CORRECT 1
(integer) 1
```

The whole client is this:

```python
action, arg = r.execute_command("GPREDICT", key, "TEXT", text)
if action == b"ACCEPT":
    return arg                              # the gate answered
label = teacher(text)                       # ABSTAIN or AUDIT: ask the teacher
if arg is not None:
    r.execute_command("GLEARN", key, arg, "LABEL", label)
return label
```

`examples/gate_client.py` wraps this; `examples/demo.py` runs both kinds of
gate against a simulated teacher and reports what was saved.

## Commands

| Command | What it does |
|---|---|
| `GCREATE key ALPHA alpha CLASSES count label ...` | create a gate that classifies text |
| `GCREATE key ALPHA alpha SCORE` | create a gate over an external score |
| `GPREDICT key TEXT text [FEATURES count name value ...]` | ask a classifier gate |
| `GPREDICT key SCORE score` | ask a score gate |
| `GLEARN key id LABEL label` / `GLEARN key id CORRECT 0\|1` | report what the teacher said |
| `GADD key TEXT text [FEATURES ...] LABEL label` | teach one example directly |
| `GINFO key` | state, thresholds, counters |
| `GPAUSE key` / `GRESUME key` | stop / resume answering (the gate keeps learning) |
| `GRESET key` | forget everything learned, keep the configuration |

Optional `GCREATE` settings: `AUDIT rate` (share of answerable requests still
sent to the teacher, default 0.05), `DELTA delta` (default 0.01),
`ALWAYSDEFER count label ...` (never answer with these labels),
`CLASSALPHA count label alpha ...` (a stricter error rate for some labels).

`GPREDICT` replies with two elements:

| Reply | Do this |
|---|---|
| `ACCEPT` *label* | use the label, skip the teacher |
| `ABSTAIN` *id* or `AUDIT` *id* | call the teacher, then `GLEARN` with the id |
| `ABSTAIN` *null* | call the teacher; no feedback wanted |

If Redis is unreachable or replies with an error, call the teacher.

## Operating notes

* A gate is one key. `GPREDICT` is a write command and runs on the primary.
* Replicas and the AOF receive what actually happened, never the command, so
  nothing random is ever replayed. An `ACCEPT` writes nothing.
* After a restart or a failover a gate keeps its model but proves itself
  again before answering.
* Send `GLEARN` for every id you are given, once the teacher has answered.
  If the teacher fails, send nothing: an id that gets no `GLEARN` within
  `gate.feedback-timeout` counts as a wrong guess, which is the safe
  reading. If no answers arrive at all, the gate stops answering by itself
  and resumes when they do.
* Keep gates out of reach of eviction (`noeviction` or a `volatile-*`
  policy): an evicted gate is simply gone.
* Suggested ACLs: application servers `+gpredict +glearn`, operators the
  rest. All commands are in the `@gate` category. Whoever may call `GLEARN`
  can teach the gate anything.
* `INFO gate` shows `gate_apply_mismatches`: replicated operations that did
  not fit the gate they were sent to. It should stay 0.
* Two server settings are read when a gate is created:
  `gate.snapshot-examples` (default 1000) and `gate.feedback-timeout`
  (milliseconds, default 60000; use about ten times your teacher's usual
  response time, or more).
* Size: a score gate takes about 36 KB, a classifier gate about 32 KB per
  label and model copy (up to three copies), plus what waits for feedback.

## Tests

```sh
make -C modules/gate test-unit    # C unit tests of every layer
make -C modules/gate sim          # Monte Carlo of the statistics and the protocol
make -C modules/gate test         # unit tests + Redis integration tests
python3 modules/gate/tests/bench.py   # rough performance numbers
```

For tests only: `CONFIG SET gate.test-seed N` (non-zero) makes every random
draw and id come from a seeded sequence. Set it to 0 and then to N again to
restart the sequence.

## Status

See the end of `SPEC.md`.
