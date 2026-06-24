# Native Bitmap Benchmark Gate

Issue aviggiano/redis#35 tracks the DD-16 gate for native bitmap exposure and
defaults. The gate is intentionally evidence-based: native creation,
conversion, and default behavior should stay provisional until Redis-level
correctness coverage and performance data show the tradeoffs clearly.

This PR covers the harness and evidence-gathering side of issue #35. It does
not close the final threshold decision; that still requires reviewing captured
benchmark artifacts and setting or revising the pass/fail criteria.

## What the Gate Measures

Use `tools/bitmap-bench.py` to produce comparable rows for:

- Sparse key creation through `SETBIT`.
- String-to-native conversion through default-Roaring writes.
- `GETBIT`, `BITCOUNT`, ranged `BITCOUNT`, `BITPOS 1`, and `BITPOS 0`.
- `BITFIELD_RO GET` and `BITFIELD SET`.
- Mixed legacy/native `BITOP AND`, `OR`, `XOR`, `NOT`, `DIFF1`, and `ONE`.
- `DUMP` / `RESTORE`, RDB save/load, and AOF rewrite.
- Memory usage, peak memory, payload size, elapsed time, repeated-run medians,
  and optional PING-canary stall estimates.

The synthetic datasets cover dense, sparse high-offset, clustered/run-heavy,
and mixed legacy/native inputs. The harness can also load CRoaring realdata
integer-set files from a local directory or download archives from
`RoaringBitmap/real-roaring-datasets`.

## Manual CI Workflow

Run the `Bitmap Benchmark` workflow manually. Set `runner_label` to the largest
configured Linux x64 runner available to the repository or organization. The
workflow builds a baseline ref and a candidate ref, then runs:

- `redis_before`: baseline build in legacy string mode.
- `redis_pr_native`: candidate build with native bitmap behavior.
- `redis_pr_legacy`: candidate build in legacy mode as a guardrail.

For publishable comparisons, enter full commit SHAs for `before_ref` and
`after_ref` instead of branch names. Branch refs are still useful for local or
exploratory runs, but the uploaded `runner.txt` records both the requested
inputs (`before_ref_input`, `after_ref_input`) and the resolved checkouts
(`before_sha`, `after_sha`). The compare JSON and Markdown also include each
run's actual `source_sha`.

The workflow uploads JSON, CSV, Markdown, and runner metadata artifacts under
`bitmap-bench-results`.

## Local Examples

Run the candidate build only:

```sh
python3 tools/bitmap-bench.py \
  --src-dir ./src \
  --mode native \
  --runs 3 \
  --ping-canary \
  --json-out bitmap-bench.json \
  --csv-out bitmap-bench.csv \
  --markdown-out bitmap-bench.md
```

Compare explicit source trees:

```sh
python3 tools/bitmap-bench.py \
  --compare-before-src-dir /path/to/before/src \
  --compare-after-src-dir /path/to/after/src \
  --compare-legacy-src-dir /path/to/after/src \
  --compare-out bitmap-bench-compare \
  --runs 3 \
  --ping-canary \
  --croaring-realdata-dir /tmp/croaring-realdata \
  --download-croaring-realdata
```

Keep developer runs small with `--request-scale 0.05`, `--skip-persistence`, or
`--only workload_a,workload_b`. Use full-scale workflow runs for data intended
to justify exposure/default decisions.

Compare output reports `native_delta_percent` as a metric-aware improvement:
positive means higher QPS for throughput rows and lower `elapsed_ms` for
latency or persistence rows.

## Decision Questions

The published table should make these questions answerable:

- Does native bitmap memory usage improve sparse and clustered datasets enough
  to justify the type split?
- Do dense datasets regress in latency, throughput, disk size, or peak memory?
- Are `BITCOUNT`, `BITPOS`, `BITFIELD`, and `BITOP` competitive on native keys?
- Are mixed legacy/native paths acceptable, especially dense mixed inputs?
- How expensive are creation, conversion, `RESTORE`, load, and AOF rewrite in
  elapsed time, peak memory, and event-loop stall estimates?
- Does candidate legacy mode preserve the old string-bitmap path closely enough?

The gate does not set permanent numeric thresholds by itself. It supplies the
repeatable evidence needed to set or revise those thresholds before exposing
native bitmap behavior by default.
