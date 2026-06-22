# Redis Roaring Native Bitmap Design Notes

> **Upstream-alignment update**: the implementation has moved to 64-bit
> indexing (`roaring64_bitmap_t`, offsets up to 2^63-9), a single
> `bitmap-default-roaring` boolean config replacing the threshold-based
> selection configs, an explicit `BITMAP CONVERT` command, and native BITOP
> destinations whenever a source is native (and always when
> `bitmap-default-roaring yes` is set). See the "Upstream-Alignment Update"
> section of
> `docs/redis-roaring-pr-breakdown.md` for the full list; where this
> document's behavior matrix disagrees, that section wins.

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
- `BITMAP CONVERT <key> NATIVE` is an observable type transition to
  `bitmap`; `BITMAP CONVERT <key> STRING` transitions back to `string`.
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

## Redis-Roaring Command Inventory

redis-roaring registers two module data types: `reroaring` for 32-bit `R.*`
keys and `roaring64` for 64-bit `R64.*` keys. Redis core v1 intentionally does
not add compatibility command names. Existing Redis bitmap commands already
cover `R.SETBIT` / `R64.SETBIT`, `R.GETBIT` / `R64.GETBIT`,
`R.BITCOUNT` / `R64.BITCOUNT`, `R.BITPOS` / `R64.BITPOS`, and the
algebra semantics of `R.BITOP` / `R64.BITOP`; in this branch, `BITOP` also
covers the redis-roaring algebra variants `DIFF`, `DIFF1`, `ANDOR`, and `ONE`.
That coverage is semantic rather than wire-compatible: redis-roaring's `NOT`
form accepts an optional `last` bound, and redis-roaring `BITOP` replies with
result cardinality instead of Redis' destination byte length. Those syntax and
reply differences are migration-tool/replay concerns, not v1 Redis command
surface. The standalone `R.DIFF` / `R64.DIFF` command names are therefore
compatibility wrappers around behavior covered by `BITOP DIFF` and remain out of
v1 Redis scope.

| redis-roaring-only family | Gap versus Redis bitmap commands | Classification | v1 migration/import note |
| --- | --- | --- | --- |
| `R.SETINTARRAY`, `R.GETINTARRAY`, `R.RANGEINTARRAY`, `R.APPENDINTARRAY`, `R.DELETEINTARRAY`, and `R64.*` equivalents | Treat set bits as sorted integer arrays, including range paging and append/delete mutations. | Migration-tool-only | Required. Export should stream set offsets from `reroaring` and `roaring64` keys; import should build native 64-bit bitmaps from 32-bit or 64-bit integer arrays. `R.RANGEINTARRAY` / `R64.RANGEINTARRAY` are useful for paged command-based export. |
| `R.SETBITARRAY`, `R.GETBITARRAY`, and `R64.*` equivalents | Use ASCII `0`/`1` bit-array strings rather than Redis raw bitmap strings. | Migration-tool-only | Optional compatibility format for tooling. Import can translate ASCII bit arrays to set offsets or native payloads; Redis core should prefer raw strings plus `BITMAP CONVERT` where a string representation fits. |
| `R.SETRANGE`, `R.SETFULL`, and `R64.*` equivalents | Create dense ranges or full universes of set bits. | Not needed for v1 | State migration can export the final set bits; command replay, if a tool supports it, can translate ranges outside Redis core. |
| `R.GETBITS`, `R.CLEARBITS`, and `R64.*` equivalents | Bulk `GETBIT`, bulk clear-to-zero, and an optional cleared-count reply. | Future Redis command candidate | Useful as generic batch bitmap operations, but not required to migrate stored values. Replay tooling can lower `CLEARBITS` to repeated clears or direct payload edits. |
| `R.CLEAR` and `R64.CLEAR` | Clear a module key without deleting the key name. | Not needed for v1 | Use `DEL` or an empty native bitmap during migration; no compatibility command is needed. |
| `R.MIN`, `R.MAX`, and `R64.*` equivalents | Return the first or last set integer. | Future Redis command candidate | `BITPOS key 1` covers the minimum; a reverse set-bit lookup could be considered later for maximum. Not required for import/export. |
| `R.CONTAINS`, `R.JACCARD`, and `R64.*` equivalents | Set-relation and similarity queries over two bitmaps. | Future Redis command candidate | Can be emulated for validation with `BITOP` plus `BITCOUNT` and temporary keys. Not required for state migration. |
| `R.OPTIMIZE` and `R64.OPTIMIZE` | Force CRoaring container optimization. | Not needed for v1 | Native Redis owns encoding optimization; migration tooling may optimize generated payloads internally. |
| `R.STAT` | Return module/container statistics for `reroaring` and `roaring64` keys. | Not needed for v1 | Use `MEMORY USAGE`, `OBJECT ENCODING`, benchmark tooling, or debug-only inspection for native stats if needed. |
| Module payloads: `reroaring`, `roaring64` | Module RDB / `DUMP` payloads are not native Redis bitmap payloads. | Migration-tool-only | Required. Tooling must read both module type names: `reroaring` uses CRoaring 32-bit serialized payloads and should be promoted to native `roaring64_bitmap_t`; `roaring64` uses CRoaring 64-bit portable payloads and can be validated/transcoded to the native bitmap RDB / `DUMP` representation. Values beyond native bitmap offset limits must fail migration or follow an explicit tool policy. Redis core must not load these module payloads directly. |

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

This original threshold-based sketch is superseded by the upstream-aligned
surface: a single `bitmap-default-roaring yes|no` flag, defaulting to `no`.
When it is `no`, bitmap writes keep creating strings unless conversion is
explicit. When it is `yes`, bitmap write commands create missing keys as
native Roaring bitmaps and convert existing string values before writing. The
size/saving thresholds and trial encodes are intentionally omitted from the
current design.

## Oracle Test Strategy

Step 1 added a legacy-string oracle harness under `tests/support`. Later steps
extended it with a `native-roaring` mode backed by `bitmap-default-roaring yes`,
so scenarios now compare bitmap-observable command results across both
representations.

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
  default. Decided in the upstream-aligned surface: `BITMAP CONVERT <key>
  [NATIVE|STRING]` is the explicit conversion command, and
  `BITMAP CONVERT <key> STRING` is the supported path back to a legacy string
  while the logical length fits proto-max-bulk-len. `BITOP` is not a string
  materialization escape hatch; destinations follow the native destination rule
  recorded in `docs/redis-roaring-pr-breakdown.md`. Plain `SET` overwrites a
  native bitmap key with a string like any other type. Generic string commands
  keep returning `WRONGTYPE`.
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
