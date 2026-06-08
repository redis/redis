# Redis Roaring Native Bitmap PR Breakdown

This branch tracks the proposed breakdown for adding a native Redis bitmap
value type that can use Roaring compression behind the existing Redis bitmap
command API.

Source plan: https://gist.github.com/aviggiano/88c437f5db7e63d30e35d567f7da72ed

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

Before that gate is satisfied, native bitmap objects may be reachable only by
test-only helpers or internal fixtures that also exercise the safety paths.

## PR 1: Design, Behavior Matrix, Test/Fuzz Scaffolding

- Add design specification.
- Add open questions.
- Add Redis bitmap/string behavior matrix.
- Add legacy-string-vs-native-bitmap oracle test harness.
- Start porting applicable redis-roaring tests/fuzz corpora.
- Document the native bitmap exposure gate.
- No behavior changes.

## PR 2: CRoaring Dependency

- Vendor CRoaring.
- Build via Redis Makefiles only.
- Add allocator/memory hook with correct aligned allocation.
- Add license/vendor notices.
- No Redis object behavior changes.

## PR 3: Native Bitmap Type Safety Plumbing

- Add `OBJ_BITMAP` using an available object type value.
- Add bitmap encodings, including Roaring.
- Add `bitmapObject` / `bitmapRoaring` lifecycle, free, copy, memory accounting,
  and active defrag handling.
- Add native bitmap persistence support before public creation is possible:
  RDB, AOF rewrite, `DUMP`, `RESTORE`, and replication.
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

## PR 4: Bitmap Command Coverage Before Public Creation

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

## PR 5: Minimal Configs and Public Native Bitmap Creation

- Add first-pass configs only:
  - `bitmap-roaring-enabled`;
  - optionally `bitmap-roaring-auto-convert`;
  - `bitmap-roaring-min-bytes`;
  - `bitmap-roaring-min-saving`.
- Enable direct `OBJ_BITMAP` creation for eligible large sparse `SETBIT` cases.
- Enable configured auto-conversion only after persistence, introspection,
  active defrag, and bitmap command coverage are already in place.
- Add tests proving converted keys survive save/load, AOF rewrite, replication,
  and the full existing bitmap command surface.

## PR 6: Optimized BITCOUNT / BITPOS

- Factor shared range normalization.
- Replace materialization fallback with optimized Roaring-aware `BITCOUNT`.
- Replace materialization fallback with optimized Roaring-aware `BITPOS`.
- Add raw oracle tests for edge cases.

## PR 7: String / Bitmap Command Boundary

- Ensure non-bitmap string commands return `WRONGTYPE` for `OBJ_BITMAP`.
- Ensure legacy `OBJ_STRING` bitmap values retain existing string behavior until
  explicitly converted.
- Audit module/string APIs for the new type.
- Decide whether Redis needs an explicit bitmap-to-string conversion escape
  hatch; keep it out of implicit generic string command behavior.

## PR 8: Optimized BITFIELD / BITFIELD_RO

- Replace materialization fallback with direct read support where practical.
- Add direct write support where practical, or keep command-local materialization
  followed by normal bitmap encoding selection.
- Preserve existing Redis `BITFIELD` behavior exactly.

## PR 9: Optimized BITOP

- Replace materialization fallback with optimized mixed legacy string bitmap /
  native bitmap `BITOP`.
- Cover all existing Redis operations, including extended operations in
  unstable.
- Add destination aliasing tests.
- Add fuzz comparison against native-conversion-disabled execution.

## PR 10: Benchmarks and Migration Tooling

- Benchmark memory, latency, disk size, rewrite time, load time, and peak memory.
- Use benchmark data to justify initial config defaults.
- Keep redis-roaring migration tooling separate from Redis core.
- Export redis-roaring keys from old Redis with module loaded.
- Import into new Redis using native Redis bitmap-compatible representation.
- Include downtime-oriented migration docs and validation tooling.
