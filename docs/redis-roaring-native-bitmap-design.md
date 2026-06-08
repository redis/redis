# Redis Roaring Native Bitmap Design Notes

This document captures the PR1 design baseline for adding Roaring compression as
part of a native Redis bitmap value type. PR1 intentionally adds no command or
storage behavior changes; it documents the expected semantics and adds oracle
tests that later PRs can reuse.

## Goals

- Keep the existing Redis bitmap command surface: `SETBIT`, `GETBIT`,
  `BITCOUNT`, `BITPOS`, `BITOP`, `BITFIELD`, and `BITFIELD_RO`.
- Add a native `OBJ_BITMAP` type in later PRs for values that opt in to bitmap
  conversion.
- Allow native bitmap values to use a Roaring-backed internal encoding.
- Preserve all existing behavior for legacy bitmap values that remain
  `OBJ_STRING`.
- Make the string-vs-bitmap command boundary explicit instead of treating
  Roaring compression as invisible string compression.
- Avoid adding user-facing redis-roaring module command prefixes such as `R.*`
  or `R64.*` to Redis core.

## Non-Goals

- PR1 does not vendor CRoaring.
- PR1 does not add `OBJ_BITMAP`, new encodings, configs, RDB/AOF formats, or
  command handlers.
- PR1 does not enable public commands to create native bitmap values. Public
  creation must wait until persistence, introspection, active defrag, and bitmap
  command coverage are already in place.
- Redis core does not import legacy redis-roaring module RDB payloads. Migration
  from module-backed deployments should remain external tooling.

## Proposed Type Model

Existing Redis bitmap commands operate on string objects. The native bitmap work
keeps those legacy values valid and introduces a type split only after a value
is explicitly allowed to become a native bitmap.

- Legacy bitmap values remain `OBJ_STRING`.
- Native bitmap values use `OBJ_BITMAP`.
- `TYPE key` returns `string` for legacy bitmap strings.
- `TYPE key` returns `bitmap` for native bitmap values.
- `OBJECT ENCODING key` may return `bitmap-roaring` or another bitmap encoding
  name for native bitmap values.

## Behavior Matrix

| Operation class | Legacy `OBJ_STRING` bitmap | Native `OBJ_BITMAP` target behavior | First implementation PR |
| --- | --- | --- | --- |
| `TYPE` | Returns `string`. | Returns `bitmap`. | PR3 |
| `OBJECT ENCODING` | Existing string encodings. | Bitmap encoding name, such as `bitmap-roaring`. | PR3 |
| `SETBIT` / `GETBIT` | Existing Redis behavior. | Same bitmap-observable behavior. | PR4 |
| `BITCOUNT` / `BITPOS` | Existing Redis behavior. | Same bitmap-observable behavior, initially via fallback if needed. | PR4 / PR6 |
| `BITFIELD` / `BITFIELD_RO` | Existing Redis behavior. | Same bitmap-observable behavior, initially via fallback if needed. | PR4 / PR8 |
| `BITOP` | Existing Redis behavior. | Accept mixed legacy/native sources, initially via fallback if needed. | PR4 / PR9 |
| `GET`, `APPEND`, `SETRANGE`, `GETRANGE`, `STRLEN` | Existing string behavior. | `WRONGTYPE`, unless a command explicitly gains bitmap support. | PR7 |
| RDB / AOF / replication | Existing string persistence. | Native bitmap persistence before public creation. | PR3 |
| `DUMP` / `RESTORE` | Existing string payloads. | Native bitmap payload behavior before public creation. | PR3 |
| `TYPE`, `SCAN ... TYPE`, `COPY` | Existing string behavior. | Explicit bitmap type handling before public creation. | PR3 |
| Modules observing Redis types | Existing string behavior. | Explicit module API/type handling before public creation. | PR3 / PR7 |

## Native Bitmap Exposure Gate

The stacked branch must not create `OBJ_BITMAP` keys through public commands
until Redis can safely own those keys everywhere they may flow.

Required before public creation or auto-conversion:

- RDB, AOF rewrite, `DUMP`, `RESTORE`, and replication support for native bitmap
  values.
- `TYPE`, `SCAN ... TYPE`, `COPY`, and `RedisModule_KeyType()` support for the
  new type.
- Free, copy, memory usage, and active defrag handling for bitmap objects.
- Direct or materialization fallback support for every existing bitmap command
  that can observe a key Redis created itself.

Until this gate is satisfied, native bitmap values should be reachable only by
test-only helpers or internal fixtures that also exercise these safety paths.

## Encoding Selection Sketch

Later PRs should keep the first configuration surface small and high-signal:

- `bitmap-roaring-enabled`
- optional `bitmap-roaring-auto-convert`
- `bitmap-roaring-min-bytes`
- `bitmap-roaring-min-saving`

The selected thresholds should be justified with benchmark data before they are
made default behavior. Native conversion should be opt-in and should remain
disabled until the exposure gate is satisfied.

## Oracle Test Strategy

PR1 adds a legacy-string oracle harness under `tests/support`. Today the harness
has only one active mode, `legacy-string`, because no native bitmap type exists
yet. Later PRs can add a native mode to the same harness and compare
bitmap-observable command results across both modes.

The initial corpus focuses on cases redis-roaring-style implementations commonly
stress:

- Sparse sets with large offsets.
- Byte, word, and Roaring-container boundary offsets.
- Repeated deterministic fuzz writes followed by `GETBIT` and `BITCOUNT`.
- String command boundary checks for values that are still legacy Redis strings.

## Open Questions

- Should native Redis bitmaps keep Redis string bitmap offset limits, or should
  the new type introduce a documented 64-bit bitmap index model?
- Which commands, if any, should offer explicit native bitmap to string
  materialization? Generic string commands should not silently materialize by
  default.
- What is the RDB opcode and AOF rewrite representation for compressed native
  bitmaps, and which PR lands it before public creation?
- Should replication send native bitmap payloads directly, or should it use a
  command sequence until the persistence format is stable?
- What debug or test-only hook should force native bitmap conversion without
  exposing production-only command surface area?
- Which Redis module APIs need a new bitmap type contract, and which should
  observe native bitmap values only as non-string values?
