# Redis Roaring Native Bitmap PR Breakdown

This branch tracks the proposed breakdown for adding a native Redis bitmap
value type that can use Roaring compression behind the existing Redis bitmap
command API.

## Upstream-Alignment Update (supersedes decisions recorded below)

The upstream issue discussion (redis/redis#15296) converged on a different
surface than several decisions recorded in the step narratives below. The
implementation now follows the upstream consensus; where the text below
contradicts this section, this section wins:

- **64-bit internal indexing**: native bitmaps store a CRoaring
  `roaring64_bitmap_t` and can represent Redis-native bitmap offsets up to
  2^63-9 (logical lengths up to 2^60-1 bytes). The public Redis bitmap command
  surface still applies Redis argument parsing and representation limits; this
  is semantic coverage for Redis-representable offsets, not a wire-compatible
  replay of every `R64.*` offset accepted by redis-roaring. The "fixed 512MB
  cap" and 32-bit `roaring_bitmap_t` choice recorded under Steps 1-3 are
  superseded. The RDB payload is the Roaring 64-bit portable format under the
  same `RDB_TYPE_BITMAP` id (the 32-bit format never shipped).
- **Offset semantics split**: read-only commands accept offsets within Redis'
  parser/native representability limits against either representation (bits
  past the end read as 0); write commands can use the native bitmap limit only
  when the target is native, while string writes keep the proto-max-bulk-len
  bound and its out-of-range error.
- **Default Roaring opt-in**: the `bitmap-roaring-{enabled,auto-convert,
  min-bytes,min-saving}` configs are replaced by a single
  `bitmap-default-roaring` boolean flag (`no` by default, or `yes`). With
  `no`, plain writes never create native bitmaps unless conversion is explicit.
  With `yes`, bitmap-command writes create new keys as native and convert
  string values that bitmap writes touch, unconditionally (the size/saving
  thresholds and conversion amortization are gone along with the trial encodes
  they amortized).
- **Explicit conversion command**: `BITMAP CONVERT <key> [NATIVE|STRING]`
  replaces both the "no new command" Step 5 escape-hatch decision and the
  test-only `DEBUG BITMAP-FORCE-ROARING`. The BITOP-copy escape hatch is
  gone (see next point); `BITMAP CONVERT key STRING` is the supported path
  back to a string, valid while the logical length fits proto-max-bulk-len.
- **BITOP destination rule**: a BITOP destination is native when at least
  one source is native, and always native when `bitmap-default-roaring yes`.
  The operation runs entirely in roaring space (no materialization), so
  64-bit sources work; only `BITOP NOT`, which is inherently dense, is
  rejected when the source's logical length exceeds proto-max-bulk-len.
  When the destination type decision depends on the local config (all-string
  sources with `bitmap-default-roaring yes`) the result propagates as an
  explicit RESTORE.

The determinism invariant is unchanged and now covers the new paths: type
transitions always reach replicas and the AOF as explicit RESTOREs of the
resulting value, never as the triggering command.

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
  hatch; keep it out of generic string command behavior. Decision:
  `BITMAP CONVERT <key> [NATIVE|STRING]` is the explicit conversion command,
  and `BITMAP CONVERT <key> STRING` is the supported path back to a legacy
  string while the logical length fits proto-max-bulk-len. `BITOP` is not a
  string materialization escape hatch; destinations are native whenever any
  source is native, and are also native for string-only sources when
  `bitmap-default-roaring yes` is set. Plain `SET` overwrites a native bitmap
  key with a string like any other type. Generic string commands keep returning
  `WRONGTYPE`.

## Step 6: Minimal Configs and Public Native Bitmap Creation

- The original threshold-based config sketch from this step is superseded.
  The upstream-aligned surface is a single `bitmap-default-roaring yes|no`
  flag, defaulting to `no`.
- With `bitmap-default-roaring no`, bitmap writes preserve string bitmap
  creation unless conversion is explicit. With `yes`, bitmap write commands
  create missing keys as native Roaring bitmaps and convert existing strings
  before writing.
- The size/saving thresholds and trial encodes are intentionally omitted from
  the current design.
- Enable public bitmap writes to create `OBJ_BITMAP` values only through the
  configured default-Roaring path: with `bitmap-default-roaring yes`, missing
  bitmap-command keys are native and existing string values are converted before
  writes.
- Propagate all default-Roaring type transitions as explicit RESTOREs so
  replicas and AOF replay never re-derive the representation choice locally.
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
- Known deferred-perf items the benchmark suite must cover explicitly:
  - `BITOP` string sources convert through per-bit insertion (batched via
    `add_many`) instead of the old materialize-plus-SIMD word loops; dense
    mixed inputs need a measured comparison and possibly a density heuristic.
  - `bitmapObjectAllocSize()` walks every container with `zmalloc_size()`
    where stream/array cache an `alloc_size` field; under
    `memory_tracking_enabled` every native bitmap write pays that walk, so
    either benchmark it as acceptable or add the cached field.
  - When any `BITOP` source exceeds the 512MB native cap (raised
    `proto-max-bulk-len` only), the bytewise fallback reads native sources
    one byte at a time through the roaring lookup path instead of
    materializing each source once.
- Keep redis-roaring migration tooling separate from Redis core.
- Use the redis-roaring command inventory in
  `docs/redis-roaring-native-bitmap-design.md` to keep v1 Redis command scope
  separate from migration-tool-only compatibility work.
- Export redis-roaring keys from old Redis with module loaded.
- Import into new Redis using native Redis bitmap-compatible representation.
- Tooling must understand both module input types: `reroaring` 32-bit payloads
  and `roaring64` 64-bit payloads. Integer-array command families
  (`SETINTARRAY`, `GETINTARRAY`, `RANGEINTARRAY`, `APPENDINTARRAY`, and
  `DELETEINTARRAY`, with `R64.*` equivalents) are migration-tool concerns, not
  Redis core commands.
- Include downtime-oriented migration docs and validation tooling.

## Upstream Packaging

The stacked steps exist for fork-side incremental review. The final upstream
submission should consolidate the stack into a single PR with the design in
the PR body, following the precedent of the Array type
(redis/redis#15162, which landed as one large PR), and must exclude this
`docs/` file - upstream has no `docs/` directory.
