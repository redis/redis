# Redis Roaring Native Bitmap PR Breakdown

This branch tracks the proposed breakdown for adding a native Redis bitmap
value type that can use Roaring compression behind the existing Redis bitmap
command API.

Source plan: https://gist.github.com/aviggiano/88c437f5db7e63d30e35d567f7da72ed

## PR 1: Design, Behavior Matrix, Test/Fuzz Scaffolding

- Add design specification.
- Add open questions.
- Add Redis bitmap/string behavior matrix.
- Add legacy-string-vs-native-bitmap oracle test harness.
- Start porting applicable redis-roaring tests/fuzz corpora.
- No behavior changes.

## PR 2: CRoaring Dependency

- Vendor CRoaring.
- Build via Redis Makefiles only.
- Add allocator/memory hook with correct aligned allocation.
- Add license/vendor notices.
- No Redis object behavior changes.

## PR 3: Native Bitmap Type and Materialization Helpers

- Add `OBJ_BITMAP` using an available object type value.
- Add bitmap encodings, including Roaring.
- Add `bitmapObject` / `bitmapRoaring` lifecycle, free, copy, and memory
  accounting.
- Add legacy string bitmap -> native bitmap conversion helpers.
- Add native bitmap -> raw bytes materialization helpers for persistence and
  tests.
- Add exact materialization tests.
- Add command type-check helpers so bitmap commands accept both strings and
  bitmaps.
- Possibly add test-only force-type/force-encoding helper.

## PR 4: SETBIT / GETBIT and Minimal Configs

- Add first-pass configs only:
  - `bitmap-roaring-enabled`;
  - optionally `bitmap-roaring-auto-convert`;
  - `bitmap-roaring-min-bytes`;
  - `bitmap-roaring-min-saving`.
- Implement Roaring-aware `SETBIT`.
- Implement Roaring-aware `GETBIT`.
- Support direct `OBJ_BITMAP` creation for large sparse `SETBIT`.
- Preserve `bytelen` and trailing-zero semantics.
- Add focused string-vs-bitmap type and encoding tests.

## PR 5: BITCOUNT / BITPOS

- Factor shared range normalization.
- Implement Roaring-aware `BITCOUNT`.
- Implement Roaring-aware `BITPOS`.
- Add raw oracle tests for edge cases.

## PR 6: String / Bitmap Command Boundary

- Ensure non-bitmap string commands return `WRONGTYPE` for `OBJ_BITMAP`.
- Ensure legacy `OBJ_STRING` bitmap values retain existing string behavior until
  explicitly converted.
- Audit module/string APIs and key introspection for the new type.
- Decide whether Redis needs an explicit bitmap-to-string conversion escape
  hatch; keep it out of implicit generic string command behavior.

## PR 7: BITFIELD / BITFIELD_RO

- Temporary or direct read path for read-only `GET` operations.
- Direct write path where practical, or command-local materialization for `SET`
  / `INCRBY` followed by normal bitmap encoding selection.
- Preserve existing Redis `BITFIELD` behavior exactly.

## PR 8: BITOP

- Implement mixed legacy string bitmap / native bitmap `BITOP`.
- Cover all existing Redis operations, including extended operations in
  unstable.
- Add destination aliasing tests.
- Add fuzz comparison against native-conversion-disabled execution.

## PR 9: Persistence Format

- Resolve compressed persistence open question.
- Implement selected persistence path.
- Add RDB/AOF/DUMP/RESTORE/replication tests.
- Benchmark disk size, rewrite time, load time, and peak memory.

## External Migration Tooling

- Separate from Redis core.
- Export redis-roaring keys from old Redis with module loaded.
- Import into new Redis using native Redis bitmap/string-compatible
  representation.
- Include downtime-oriented migration docs and validation tooling.
