# Redis Roaring Native Bitmap Design Notes

This document captures the Step 1 design baseline for adding Roaring
compression as part of a native Redis bitmap value type. Step numbers refer to
the plan phases in `docs/redis-roaring-pr-breakdown.md` (numbered Step 0-10;
GitHub PR numbers in this fork drift from plan phases because issues and pull
requests share one number counter - see the breakdown document for the current
mapping). Step 1 intentionally adds no command or storage behavior changes; it
documents the expected semantics and adds oracle tests that later steps can
reuse.

## Goals

- Keep the existing Redis bitmap command surface: `SETBIT`, `GETBIT`,
  `BITCOUNT`, `BITPOS`, `BITOP`, `BITFIELD`, and `BITFIELD_RO`.
- Add a native `OBJ_BITMAP` type in later steps for values that opt in to
  bitmap conversion.
- Allow native bitmap values to use a Roaring-backed internal encoding.
- Preserve all existing behavior for legacy bitmap values that remain
  `OBJ_STRING`.
- Make the string-vs-bitmap command boundary explicit instead of treating
  Roaring compression as invisible string compression.
- Avoid adding user-facing redis-roaring module command prefixes such as `R.*`
  or `R64.*` to Redis core.

## Non-Goals

- Step 1 does not vendor CRoaring.
- Step 1 does not add `OBJ_BITMAP`, new encodings, configs, RDB/AOF formats, or
  command handlers.
- Step 1 does not enable public commands to create native bitmap values. Public
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

| Operation class | Legacy `OBJ_STRING` bitmap | Native `OBJ_BITMAP` target behavior | First implementation step |
| --- | --- | --- | --- |
| `TYPE` | Returns `string`. | Returns `bitmap`. | Step 3 |
| `OBJECT ENCODING` | Existing string encodings. | Bitmap encoding name, such as `bitmap-roaring`. | Step 3 |
| `SETBIT` / `GETBIT` | Existing Redis behavior. | Same bitmap-observable behavior. | Step 4 |
| `BITCOUNT` / `BITPOS` | Existing Redis behavior. | Same bitmap-observable behavior, initially via fallback if needed. | Step 4 / Step 7 |
| `BITFIELD` / `BITFIELD_RO` | Existing Redis behavior. | Same bitmap-observable behavior, initially via fallback if needed. | Step 4 / Step 8 |
| `BITOP` | Existing Redis behavior. | Accept mixed legacy/native sources, initially via fallback if needed. | Step 4 / Step 9 |
| `GET`, `APPEND`, `SETRANGE`, `GETRANGE`, `STRLEN` | Existing string behavior. | `WRONGTYPE`, unless a command explicitly gains bitmap support. | Step 5 |
| RDB / AOF / replication | Existing string persistence. | Native bitmap persistence before public creation. | Step 3 |
| `DUMP` / `RESTORE` | Existing string payloads. | Native bitmap payload behavior before public creation. | Step 3 |
| `TYPE`, `SCAN ... TYPE`, `COPY` | Existing string behavior. | Explicit bitmap type handling before public creation. | Step 3 |
| Modules observing Redis types | Existing string behavior. | Explicit module API/type handling before public creation. | Step 3 / Step 5 |

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
- Type transitions (creation and conversion) propagate explicitly and
  deterministically to replicas and AOF. Conversion decisions must be pure
  functions of replicated logical state (logical byte length, cardinality);
  replicas never re-derive type decisions from local config, allocator
  measurements such as `zmalloc_size()`, or build differences.

The gate also carries a test bar (legacy-string oracle parity, conversion
invariants, save/load + replication + AOF round-trips, corrupt-`RESTORE`
coverage) and a performance bar (no unbounded event-loop stalls from
materialization fallbacks or conversion-eligibility checks).

Until this gate is satisfied, native bitmap values should be reachable only by
test-only helpers or internal fixtures that also exercise these safety paths.

## Encoding Selection Sketch

Later steps should keep the first configuration surface small and high-signal:

- `bitmap-roaring-enabled`
- optional `bitmap-roaring-auto-convert`
- `bitmap-roaring-min-bytes`
- `bitmap-roaring-min-saving`

The selected thresholds should be justified with benchmark data before they are
made default behavior. Native conversion should be opt-in and should remain
disabled until the exposure gate is satisfied. Defaults shipped before the
Step 10 benchmark report exist only as provisional values, and
`bitmap-roaring-auto-convert` stays `no` until that report exists and the
Step 0 auto-convert question is answered upstream.

## Oracle Test Strategy

Step 1 adds a legacy-string oracle harness under `tests/support`. Today the
harness has only one active mode, `legacy-string`, because no native bitmap
type exists yet. Later steps can add a native mode to the same harness and
compare bitmap-observable command results across both modes.

The initial corpus focuses on cases redis-roaring-style implementations commonly
stress:

- Sparse sets with large offsets.
- Byte, word, and Roaring-container boundary offsets.
- Repeated deterministic fuzz writes followed by `GETBIT` and `BITCOUNT`.
- String command boundary checks for values that are still legacy Redis strings.

## Open Questions

- Should native Redis bitmaps keep Redis string bitmap offset limits, or should
  the new type introduce a documented 64-bit bitmap index model? This decision
  is owned by Step 1 and must be answered before the Step 3 RDB payload and
  object type id are submitted upstream; a later 64-bit variant would take a
  new RDB type id (for example `RDB_TYPE_BITMAP_64`) per core convention, not
  an in-payload flags byte.
- Which commands, if any, should offer explicit native bitmap to string
  materialization? Generic string commands should not silently materialize by
  default. Decided in Step 5: no dedicated conversion command. `BITOP` already
  provides an explicit, copying escape hatch because `BITOP` destinations are
  always stored as plain strings, and plain `SET` overwrites a native bitmap
  key with a string like any other type. Generic string commands keep
  returning `WRONGTYPE`. Revisit only if upstream review asks for a dedicated
  command.
- What is the RDB opcode and AOF rewrite representation for compressed native
  bitmaps, and which PR lands it before public creation? Decided in Step 3:
  RDB uses a dedicated `RDB_TYPE_BITMAP` id carrying the portable Roaring
  payload (a future format variant takes a new RDB type id, not an in-payload
  flags byte), and AOF rewrite emits `RESTORE <key> 0 <DUMP payload> REPLACE`
  for native bitmap values. Both land before Step 6 public creation.
- Should replication send native bitmap payloads directly, or should it use a
  command sequence until the persistence format is stable? Decided in Step 6:
  type transitions replicate as an explicit
  `RESTORE <key> <ttl> <DUMP payload> REPLACE [ABSTTL]` rewrite of the
  triggering command, so type-transition decisions stay pure functions of
  replicated logical state and propagate explicitly (see the exposure gate);
  replicas never re-derive them from local config or allocator state.
  Post-transition writes replicate verbatim against the same type on both
  sides.
- What debug or test-only hook should force native bitmap conversion without
  exposing production-only command surface area? Decided in Step 3:
  `DEBUG BITMAP-FORCE-ROARING <key>` (with `DEBUG BITMAP-RAW` for byte-exact
  reads); it propagates its effect as the same RESTORE rewrite used by
  SETBIT-triggered conversions, since replicas may refuse DEBUG entirely.
- Which Redis module APIs need a new bitmap type contract, and which should
  observe native bitmap values only as non-string values?
