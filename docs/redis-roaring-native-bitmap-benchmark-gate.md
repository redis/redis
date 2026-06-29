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
- `redis_roaring_module`: `aviggiano/redis-roaring` loaded into a
  `redis/redis` host build.
- `redis_pr_legacy`: candidate build in legacy mode as a guardrail.

For publishable comparisons, enter full commit SHAs for `baseline_redis_ref`,
`core_redis_ref`, `module_host_redis_ref`, and `redis_roaring_ref` instead of
branch names. Branch refs are still useful for local or exploratory runs, but
the uploaded `runner.txt` records both the requested inputs and the resolved
checkouts. The compare JSON includes each Redis run's actual `source_sha`,
plus `module_sha` for the redis-roaring module run. The compare Markdown links
the run labels to the resolved commits and links the module commit when present.

The workflow uploads JSON, CSV, Markdown, and runner metadata artifacts under
`bitmap-bench-results`.

Use `benchmark_profile` to choose the workload family:

- `small-sets`: sparse/high-offset integer-set-like datasets and CRoaring
  realdata inputs.
- `bitsets`: dense, clustered, and packed-field bitmap workloads.
- `mixed-bitop`: all-string, all-native, and mixed-source BITOP workloads.
- `smoke`: short sanity profile with request floors to avoid misleading
  `0.00 qps` rows.
- `full`: all benchmark families.

Use `report_view` to publish `performance`, `memory`, `payload`,
`accounting`, or the default `combined` Markdown report. Performance rows use
`time_per_op_us` as the primary metric and keep QPS as secondary data in
JSON/CSV. Repeated samples also publish mean, median, min, max, standard
deviation, and coefficient of variation fields for `time_per_op_us`.

## Memory Accounting Audit

Issue aviggiano/redis#120 audited the native bitmap memory rows against the
`redis-roaring` module rows. The two `MEMORY USAGE` columns are not
apples-to-apples live heap measurements:

- Redis string and Redis core Roaring keys are reported by Redis core. They
  include the object/key wrapper plus the value allocations Redis owns; native
  Roaring additionally accounts the wrapper struct, CRoaring `roaring64`
  bitmap, ART node arrays, container pointer array, and each container payload.
- `redis-roaring` module keys are reported through the module data type
  `mem_usage` callback. In the audited module revision
  (`aviggiano/redis-roaring` `27b542f98f770f91abbb2d31b6f8e6574fd01f7c`), the
  32-bit callback returns `roaring_bitmap_size_in_bytes()` and the 64-bit
  callback returns `roaring64_bitmap_portable_size_in_bytes()`. Those are
  CRoaring serialized-size proxies, not a full live allocation walk.

Do not tune native Redis to match the module's callback-reported memory bytes
by under-reporting native allocations. Use the report's Storage section for
serialized representation size and the Accounting Diagnostics section for
`MEMORY USAGE` / payload ratios. This keeps real representation overhead
separate from module reporting artifacts.

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
  --compare-module-src-dir /path/to/module-host/src \
  --compare-module-path /path/to/libredis-roaring.so \
  --compare-legacy-src-dir /path/to/after/src \
  --compare-out bitmap-bench-compare \
  --benchmark-profile full \
  --report-view combined \
  --runs 3 \
  --ping-canary \
  --croaring-realdata-dir /tmp/croaring-realdata \
  --download-croaring-realdata \
  --realdata-archives census1881,census-income,uscensus2000,weather_sept_85,wikileaks-noquotes \
  --realdata-max-files 10
```

Keep developer runs small with `--request-scale 0.05`, `--skip-persistence`, or
`--only workload_a,workload_b`. Use full-scale workflow runs for data intended
to justify exposure/default decisions.

Compare output reports `native_delta_percent` as a metric-aware improvement:
positive means lower `time_per_op_us` for command rows and lower `elapsed_ms`
for persistence rows. The published Markdown table focuses on Redis string,
Redis core Roaring, and redis-roaring module columns; unsupported module rows
are marked `N/A`. The optional `redis_pr_legacy` guardrail remains available in
JSON and CSV but is omitted from the Markdown run table so the rendered report
shows the three primary comparison targets. The Markdown keeps the human-facing
tables compact by folding dataset, group, and story context into the operation
cell; JSON and CSV retain the expanded columns for analysis.

The compare Markdown is split into first-class performance, memory, and
storage sections. Dataset metadata includes bitcount, max set
offset, logical byte length, and density so sparse small-set rows are not mixed
with dense bitset rows without context.

The default workflow downloads CRoaring's `census1881` archive so the big
benchmark includes the same source dataset used by `aviggiano/redis-roaring`'s
published performance table. The workflow raises `--realdata-max-files` to 10
so census coverage is added on top of the prior two-file coverage for each
default archive. Census rows are aggregated by operation to keep the report
compact: keys follow the redis-roaring benchmark's implementation index pattern
(`0-i` for module, `1-i` for native Redis, `2-i` for Redis strings), and BITOP
rows use the same unary `NOT` and paired `AND`/`OR`/`XOR`/`ANDOR`/`ONE` source
shapes where Redis supports them.

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
