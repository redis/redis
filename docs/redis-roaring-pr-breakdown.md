# Redis Roaring Native Bitmap PR Breakdown

This branch tracks the proposed breakdown for adding a native Redis bitmap
value type that can use Roaring compression behind the existing Redis bitmap
command API.

Source plan: https://gist.github.com/aviggiano/66a88ee2d3d074df39a4228b4acec1a3

Plan phases are numbered "Step 0" through "Step 10", not "PR N". The stacked
GitHub PR numbers in this fork drift from the plan numbering because issues
and pull requests share one number counter (as of this revision: Step 1 =
PR #2, Step 2 = PR #3, Step 3 = PR #4, Step 4 = PR #5, Step 5 = PR #8,
Step 6 = PR #9, Step 7 = PR #10, Step 8 = PR #11, Step 9 = PR #12), and
using "PR N" for both invites confusion. GitHub PR numbers are written as
"PR #N" where one is meant.

## Exposure Gate

No public command may create or auto-convert a key to `OBJ_BITMAP` until all of
these are implemented in the stacked branch:

- RDB, AOF rewrite, `DUMP`, `RESTORE`, and replication handling for native
  bitmap values.
- `TYPE`, `SCAN ... TYPE`, `COPY`, and module API introspection for the new
  type.
- Free, copy, memory usage, and active defrag handling for bitmap objects.
- Fallback or direct support for every existing bitmap command that can observe
  a key Redis created itself.
- Type transitions (creation and conversion) propagate explicitly and
  deterministically to replicas and AOF. Conversion decisions must be pure
  functions of replicated logical state (logical byte length, cardinality);
  replicas never re-derive type decisions from local config, allocator
  measurements such as `zmalloc_size()`, or build differences.

The gate also carries a test bar and a performance bar:

- Test bar: legacy-string oracle parity tests, conversion invariant tests,
  save/load + replication + AOF round-trips, and corrupt-`RESTORE` coverage
  (`tests/integration/corrupt-dump.tcl` cases plus
  `tests/integration/corrupt-dump-fuzzer.tcl` and bitmap entries in
  `generate_fuzzy_traffic_on_key` in `tests/support/util.tcl`).
- Performance bar: no unbounded event-loop stalls. Materialization fallbacks
  and conversion-eligibility checks must be bounded or incremental relative to
  value size; in particular the hot write path must not pay O(value) work per
  command just to decide whether to convert.

Before that gate is satisfied, native bitmap objects may be reachable only by
test-only helpers or internal fixtures that also exercise the safety paths.

## Step 0: Upstream Alignment

- Post the design summary on redis/redis#15296 before the Step 3 RDB payload
  and object type id are submitted upstream.
- Lead with the two questions the maintainer comment raised: 64-bit indexing,
  and compressed-bitmaps-only vs automatic format switching (auto-convert).
  Include the replication/AOF determinism design for type transitions.
- Get explicit maintainer confirmation of the new-type pivot. The upstream
  issue text as written promises that `TYPE` stays `string` and generic string
  commands keep working; this plan delivers the opposite, and the existing
  maintainer comment is a suggestion, not design approval.
- Reference the upstream issue as "Refs", not "Fixes", until the confirmed
  scope actually matches the issue.

## Step 1: Design, Behavior Matrix, Test/Fuzz Scaffolding

- Add design specification.
- Add open questions.
- Add Redis bitmap/string behavior matrix.
- Add legacy-string-vs-native-bitmap oracle test harness.
- Own the 64-bit indexing decision: decide (or explicitly time-box) whether
  `OBJ_BITMAP` keeps Redis string bitmap offset limits or introduces a
  documented 64-bit index model. Hard deadline is Step 3, because the RDB
  payload and type id ossify the answer. Note the current asymmetry: string
  bitmap offsets are bounded by `proto-max-bulk-len` (a check bypassed for
  master/AOF-loading clients via `mustObeyClient`), while the in-progress
  native code hard-codes a fixed 512MB cap. If 64-bit support arrives later,
  it should be a new RDB type id (for example `RDB_TYPE_BITMAP_64`), following
  the core convention of one RDB type per format variant - not an in-payload
  index-width or flags byte, which is a module-type (`encver`) mechanism.
- Port redis-roaring test properties and edge cases, not its fuzz corpora: the
  module corpora are libFuzzer inputs for `R.*`-shaped harnesses driving a
  spawned server over hiredis, and Redis core has no libFuzzer infrastructure,
  so a literal port is not actionable. Instead, extend
  `tests/integration/corrupt-dump-fuzzer.tcl` for the native bitmap payload
  (Step 3) and add bitmap commands to `generate_fuzzy_traffic_on_key` in
  `tests/support/util.tcl`.
- Document the native bitmap exposure gate.
- No behavior changes.

## Step 2: CRoaring Dependency

- Vendor CRoaring.
- Build via Redis Makefiles only.
- Add allocator/memory hook with correct aligned allocation.
- Add license/vendor notices.
- No Redis object behavior changes.

## Step 3: Native Bitmap Type Safety Plumbing

- Add `OBJ_BITMAP` using an available object type value.
- Add bitmap encodings, including Roaring.
- Add `bitmapObject` / `bitmapRoaring` lifecycle, free, copy, memory accounting,
  and active defrag handling.
- Add native bitmap persistence support before public creation is possible:
  RDB, AOF rewrite, `DUMP`, `RESTORE`, and replication.
- AOF rewrite strategy (already decided in the implementation; document it for
  reviewers): rewrite emits `RESTORE <key> 0 <DUMP payload> REPLACE` for
  native bitmap values. This is novel for a core type and couples
  command-format AOF content to the RDB payload version even without
  `aof-use-rdb-preamble`. Alternatives considered: replaying per-bit `SETBIT`
  sequences (O(set bits), and cannot deterministically preserve trailing-zero
  length plus type selection), and a dedicated creation command (new command
  surface). Call the choice and its version coupling out explicitly in the PR
  description.
- Harden `RESTORE` for the new payload: bounds-checked deserialization
  (`roaring_bitmap_portable_deserialize_safe`) plus logical-length/max-offset
  validation unconditionally, and full structural validation
  (`roaring_bitmap_internal_validate`) under `sanitize-dump-payload` deep
  integrity validation, with corrupt-payload tests.
- The 64-bit indexing decision (Step 1) must be resolved before this step's
  RDB payload and type id are submitted upstream.
- Add key introspection and module-facing type handling:
  `TYPE`, `SCAN ... TYPE`, `COPY`, and `RedisModule_KeyType()`.
- Add legacy string bitmap -> native bitmap conversion helpers.
- Add native bitmap -> raw bytes materialization helpers for persistence,
  command fallbacks, and tests.
- Add exact materialization tests.
- Add command type-check helpers so bitmap commands can accept both strings and
  bitmaps.
- Add test-only force-type/force-encoding helpers only after the safety paths
  above are available.
- Do not enable public command creation or auto-conversion yet.

## Step 4: Bitmap Command Coverage Before Public Creation

- Add direct or materialization fallback support for native bitmap values across
  the existing bitmap command API:
  - `SETBIT`
  - `GETBIT`
  - `BITCOUNT`
  - `BITPOS`
  - `BITFIELD`
  - `BITFIELD_RO`
  - `BITOP`
- Preserve legacy `OBJ_STRING` bitmap behavior.
- Preserve `bytelen` and trailing-zero semantics.
- Add focused string-vs-bitmap type and encoding tests.
- Do not enable public command creation or auto-conversion until this coverage
  is complete.

## Step 5: String / Bitmap Command Boundary

This audit lands before Step 6 exposes public creation: audits belong before
exposure, not after. `checkType()` makes `WRONGTYPE` mostly automatic, so the
work here is auditing the surfaces that bypass or sidestep plain type checks.

- Ensure non-bitmap string commands return `WRONGTYPE` for `OBJ_BITMAP`.
- Ensure legacy `OBJ_STRING` bitmap values retain existing string behavior until
  explicitly converted.
- Audit `SORT ... BY`/`GET` patterns, module/string APIs, and Lua script
  surfaces that read values as strings.
- Decide whether Redis needs an explicit bitmap-to-string conversion escape
  hatch; keep it out of implicit generic string command behavior. Decision:
  no dedicated conversion command. `BITOP` already provides an explicit,
  copying escape hatch because `BITOP` destinations are always stored as
  plain strings (for example `BITOP OR dest src` materializes a native
  bitmap `src` into a string `dest`), and plain `SET` overwrites a native
  bitmap key with a string like any other type. Generic string commands keep
  returning `WRONGTYPE`. Revisit only if upstream review asks for a
  dedicated command.

## Step 6: Minimal Configs and Public Native Bitmap Creation

- Add first-pass configs only:
  - `bitmap-roaring-enabled`;
  - optionally `bitmap-roaring-auto-convert`;
  - `bitmap-roaring-min-bytes`;
  - `bitmap-roaring-min-saving`.
- Config defaults in this step are provisional until the Step 10 benchmark
  report exists; the report must justify or revise them before any upstream
  submission.
- `bitmap-roaring-auto-convert` defaults to `no` and stays `no` until the
  Step 10 benchmark report exists and the Step 0 auto-convert question is
  answered upstream.
- Enable direct `OBJ_BITMAP` creation for eligible large sparse `SETBIT` cases.
- Enable configured auto-conversion only after persistence, introspection,
  active defrag, and bitmap command coverage are already in place.
- Add tests proving converted keys survive save/load, AOF rewrite, replication,
  and the full existing bitmap command surface.

## Step 7: Optimized BITCOUNT / BITPOS

- Factor shared range normalization.
- Replace materialization fallback with optimized Roaring-aware `BITCOUNT`.
- Replace materialization fallback with optimized Roaring-aware `BITPOS`.
- Add raw oracle tests for edge cases.

## Step 8: Optimized BITFIELD / BITFIELD_RO

- Replace materialization fallback with direct read support where practical.
- Add direct write support where practical, or keep command-local materialization
  followed by normal bitmap encoding selection.
- Preserve existing Redis `BITFIELD` behavior exactly.

## Step 9: Optimized BITOP

- Replace materialization fallback with optimized mixed legacy string bitmap /
  native bitmap `BITOP`.
- Cover all existing Redis operations, including extended operations in
  unstable.
- Add destination aliasing tests.
- Add fuzz comparison against native-conversion-disabled execution.

## Step 10: Benchmarks and Migration Tooling

- Benchmark memory, latency, disk size, rewrite time, load time, and peak memory.
- Use benchmark data to justify the provisional Step 6 config defaults, or
  revise them.
- Keep redis-roaring migration tooling separate from Redis core.
- Export redis-roaring keys from old Redis with module loaded.
- Import into new Redis using native Redis bitmap-compatible representation.
- Include downtime-oriented migration docs and validation tooling.

## Upstream Packaging

The stacked steps exist for fork-side incremental review. The final upstream
submission should consolidate the stack into a single PR with the design in
the PR body, following the precedent of the Array type
(redis/redis#15162, which landed as one large PR), and must exclude this
`docs/` file - upstream has no `docs/` directory.
