# Redis Roaring Native Bitmap PR Breakdown

This branch tracks the proposed breakdown for adding a native Redis bitmap
value type that can use Roaring compression behind the existing Redis bitmap
command API.

## Current Draft Status (supersedes decisions recorded below)

The upstream issue discussion (redis/redis#15296) and local tracker issues have
moved several decisions since the original step narratives below were written.
This section records the current `unstable` draft implementation and local
design status; where the text below contradicts this section, this section
wins. This is not a claim of upstream maintainer approval for decisions still
marked pending in the trackers.

- **Index width and public offset cap**: the current implementation stores
  native bitmaps with CRoaring `roaring64_bitmap_t` and an explicit logical byte
  length. DD-17 settles v1 on 64-bit-capable internals with a bounded native
  surface: native bitmap logical length is capped at 512 MiB, max bit offset
  `4294967295`; writes also honor `proto-max-bulk-len`. Lifting the cap is
  deferred to a future compatibility and performance decision, and read-only
  native lookups may exceed a lower current `proto-max-bulk-len` only within the
  native cap. See [#24](https://github.com/aviggiano/redis/issues/24) and
  [#42](https://github.com/aviggiano/redis/issues/42).
- **Default Roaring opt-in**: the `bitmap-roaring-{enabled,auto-convert,
  min-bytes,min-saving}` configs are replaced by a single
  `bitmap-default-roaring` boolean flag (`no` by default, or `yes`). With
  `no`, plain writes never create native bitmaps unless conversion is explicit.
  With `yes`, bitmap-command writes create new keys as native and convert
  string values that bitmap writes touch, unconditionally (the size/saving
  thresholds and conversion amortization are gone along with the trial encodes
  they amortized).
- **Explicit conversion command**: the current draft implements
  `BITMAP CONVERT <key> [NATIVE|STRING]`. It replaces the old BITOP-copy
  escape hatch in this fork, and `BITMAP CONVERT key STRING` is the draft path
  back to a string while the logical length fits `proto-max-bulk-len`. Final
  v1 approval for this public surface remains pending in
  [#20](https://github.com/aviggiano/redis/issues/20),
  [#22](https://github.com/aviggiano/redis/issues/22), and
  [#26](https://github.com/aviggiano/redis/issues/26).
- **BITOP destination rule**: a BITOP destination is native when at least
  one source is native, and always native when `bitmap-default-roaring yes`.
  Native `BITOP` destinations are bounded by the 512 MiB native cap and by
  `proto-max-bulk-len` when it is lower: commands are rejected when the result
  logical length would exceed either limit. Most native operations run entirely
  in roaring space without materializing a Redis string; `BITOP NOT` is the
  dense/materializing case and is rejected when the source's logical length
  exceeds `proto-max-bulk-len`.
  When the destination type decision depends on the local config (all-string
  sources with `bitmap-default-roaring yes`) the result propagates as an
  explicit RESTORE.
- **Persistence payload**: the current `RDB_TYPE_BITMAP` payload stores a v2
  marker followed by encoding-specific data: raw payloads use the RDB string
  length, while range payloads keep the logical byte length. The loader also
  accepts the previous same-version layout that wrote logical byte length
  before encoding. AOF rewrite emits `RESTORE` with the DUMP payload. The DD-10
  baseline in [#29](https://github.com/aviggiano/redis/issues/29) is the V2
  raw/range payload direction with RESTORE input treated as untrusted; open
  [PR #44](https://github.com/aviggiano/redis/pull/44) is related review
  follow-up, not current `unstable` behavior.

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

The original plan used this gate to prevent public `OBJ_BITMAP` creation until
Redis could safely own native bitmap keys everywhere they may flow. The current
draft implementation exposes public creation through `bitmap-default-roaring
yes` and explicit `BITMAP CONVERT` after implementing the safety paths below;
pending design and benchmark trackers still decide whether that surface is
final.

The checklist remains:

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

Until the pending design and benchmark items are resolved, treat the exposed
surface as draft implementation status rather than final upstream-approved
design.

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
- Own the 64-bit indexing decision. The v1 decision is to keep
  `roaring64_bitmap_t` internally while bounding native bitmap values to
  512 MiB / max bit `4294967295`; future work can revisit wider native offsets
  as an explicit compatibility decision.
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
- AOF rewrite strategy: rewrite emits `RESTORE <key> 0 <DUMP payload> REPLACE`
  for native bitmap values. This is novel for a core type and couples
  command-format AOF content to the RDB payload version even without
  `aof-use-rdb-preamble`. Alternatives considered: replaying per-bit `SETBIT`
  sequences (O(set bits), and cannot deterministically preserve trailing-zero
  length plus type selection), and a dedicated creation command (new command
  surface). Call the choice and its version coupling out explicitly in the PR
  description.
- Harden `RESTORE` for the new payload: always validate logical byte length,
  raw payload length, max-offset/cardinality invariants, and canonical range
  ordering, with corrupt-payload tests. Deep CRoaring-container validation is
  not part of the raw/range payload contract.
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
  hatch; keep it out of generic string command behavior. Current draft surface:
  `BITMAP CONVERT <key> [NATIVE|STRING]` is the explicit conversion command,
  and `BITMAP CONVERT <key> STRING` is the path back to a legacy string while
  the logical length fits `proto-max-bulk-len`. `BITOP` is not a string
  materialization escape hatch; destinations are native whenever any source is
  native, and are also native for string-only sources when
  `bitmap-default-roaring yes` is set. Plain `SET` overwrites a native bitmap
  key with a string like any other type. Generic string commands keep returning
  `WRONGTYPE`; final v1 confirmation remains tracked in DD-01/DD-03/DD-07.

## Step 6: Minimal Configs and Public Native Bitmap Creation

- The original threshold-based config sketch from this step is superseded.
  The current draft surface is a single `bitmap-default-roaring yes|no` flag,
  defaulting to `no`.
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
- Use `tools/bitmap-bench.py` and the workflow documented in
  `docs/redis-roaring-native-bitmap-benchmark-gate.md` for the DD-16
  Redis-level benchmark gate.
- Treat PR #79 / issue #35 work here as harness and evidence tooling first:
  closing the final threshold/default decision still requires reviewing the
  captured benchmark artifacts and recording explicit criteria.
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
  - Materialization paths such as `BITMAP CONVERT ... STRING`,
    `DEBUG BITMAP-RAW`, and the current raw-byte persistence payload need
    benchmark coverage and explicit limits because they flatten native bitmaps.
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
