# Redis Roaring Native Bitmap Design Notes

> **Current local status**: this document tracks the draft native bitmap design
> in this fork, not upstream maintainer approval. Design status and
> implementation status are listed separately because several tracker decisions
> are still moving. The current `unstable` implementation has `OBJ_BITMAP`,
> `bitmap-roaring`, `bitmap-default-roaring`, `BITMAP CONVERT`, direct native
> bitmap command support, and RESTORE-based type-transition propagation. DD-17
> settled the v1 index-width rule: native bitmaps keep `roaring64_bitmap_t`
> internally, but their logical length is capped at 512 MiB (max bit offset
> `4294967295`). Bitmap writes also honor `proto-max-bulk-len`; read-only
> native lookups can exceed a lower current `proto-max-bulk-len` setting only
> while staying inside the native v1 cap.

This document started as the Step 1 design baseline for adding Roaring
compression as part of a native Redis bitmap value type. It now also records
the current local implementation status so reviewers can tell which behavior is
already in the fork and which design decisions still need explicit resolution.
Step numbers refer to the plan phases in
`docs/redis-roaring-pr-breakdown.md` (numbered Step 0-10; GitHub PR numbers in
this fork drift from plan phases because issues and pull requests share one
number counter - see the breakdown document for the current mapping).

## Goals

- Keep the existing Redis bitmap command surface: `SETBIT`, `GETBIT`,
  `BITCOUNT`, `BITPOS`, `BITOP`, `BITFIELD`, and `BITFIELD_RO`.
- Use a native `OBJ_BITMAP` type for values that opt in to bitmap conversion
  or are created through the configured native-bitmap path.
- Allow native bitmap values to use a Roaring-backed internal encoding with an
  explicit logical byte length.
- Preserve all existing behavior for legacy bitmap values that remain
  `OBJ_STRING`.
- Make the string-vs-bitmap command boundary explicit instead of treating
  Roaring compression as invisible string compression.
- Avoid adding user-facing redis-roaring module command prefixes such as `R.*`
  or `R64.*` to Redis core.

## Non-Goals

- Redis core does not import legacy redis-roaring module RDB payloads. Migration
  from module-backed deployments should remain external tooling.
- Generic string commands do not silently materialize native bitmap values back
  into strings.
- This document does not claim upstream maintainer approval for tracker issues
  marked pending.
- The status matrix is not a substitute for implementation tests, benchmarks,
  or upstream review.

## Proposed Type Model

Existing Redis bitmap commands operate on string objects. The native bitmap work
keeps those legacy values valid and introduces a type split only after a value
is explicitly allowed to become a native bitmap. Local tracker
[#21](https://github.com/aviggiano/redis/issues/21) records the observable type
model as ready; [PR #39](https://github.com/aviggiano/redis/pull/39) updated
the command-facing docs and tests for this boundary.

- Legacy bitmap values remain `OBJ_STRING`.
- Native bitmap values use `OBJ_BITMAP`.
- `TYPE key` returns `string` for legacy bitmap strings.
- `TYPE key` returns `bitmap` for native bitmap values.
- `BITMAP CONVERT <key> NATIVE` is an observable type transition to
  `bitmap`; `BITMAP CONVERT <key> STRING` transitions back to `string`.
- `OBJECT ENCODING key` returns `bitmap-roaring` for the current native
  encoding.

## Design and Implementation Status Matrix

| Area | Design status | Current implementation status | Trackers / PRs |
| --- | --- | --- | --- |
| Observable type model | Ready locally: native bitmaps are an observable `OBJ_BITMAP` / `TYPE bitmap`, not transparent string compression. | Implemented. `OBJECT ENCODING` reports `bitmap-roaring`. | [#21](https://github.com/aviggiano/redis/issues/21), [PR #39](https://github.com/aviggiano/redis/pull/39) |
| Internal encoding and logical length | Ready locally for Roaring-backed storage with explicit logical byte length and a bounded v1 native index space. | Implemented with `roaring64_bitmap_t`, `byte_len`, and a 512 MiB native logical length cap. | [#25](https://github.com/aviggiano/redis/issues/25), [PR #38](https://github.com/aviggiano/redis/pull/38) |
| Public offset and index-width model | DD-17 resolved v1 in favor of 64-bit-capable internals with the safer bounded native surface, not a switch to 32-bit Roaring. | Native bitmaps are capped at 512 MiB / max bit `4294967295`; writes also obey `proto-max-bulk-len`, and read-only native lookups may exceed a lower current `proto-max-bulk-len` only within the native cap. | [#24](https://github.com/aviggiano/redis/issues/24), [#42](https://github.com/aviggiano/redis/issues/42), [PR #43](https://github.com/aviggiano/redis/pull/43) |
| Creation and conversion controls | Draft surface is `bitmap-default-roaring yes|no` plus `BITMAP CONVERT`; final v1 confirmation remains pending. | Implemented. `no` preserves legacy string creation unless conversion is explicit; `yes` creates/converts native bitmaps on bitmap writes. | [#20](https://github.com/aviggiano/redis/issues/20), [#22](https://github.com/aviggiano/redis/issues/22), [#26](https://github.com/aviggiano/redis/issues/26), [PR #13](https://github.com/aviggiano/redis/pull/13), [PR #15](https://github.com/aviggiano/redis/pull/15) |
| Existing bitmap commands | Ready locally: existing bitmap commands must work on both legacy strings and native bitmaps without fallback in `unstable`. | Implemented for `SETBIT`, `GETBIT`, `BITCOUNT`, `BITPOS`, `BITFIELD`, `BITFIELD_RO`, and `BITOP`, with parity/oracle coverage. | [#23](https://github.com/aviggiano/redis/issues/23), merged landing PRs [PR #40](https://github.com/aviggiano/redis/pull/40) and [PR #41](https://github.com/aviggiano/redis/pull/41); historical stacked PRs later landed through [PR #40](https://github.com/aviggiano/redis/pull/40): [PR #10](https://github.com/aviggiano/redis/pull/10), [PR #11](https://github.com/aviggiano/redis/pull/11), [PR #12](https://github.com/aviggiano/redis/pull/12) |
| `BITOP` destination and dense `NOT` limits | Ready locally: mixed legacy/native `BITOP` preserves Redis return values; native destinations follow source type or `bitmap-default-roaring` and are bounded by the v1 native cap plus `proto-max-bulk-len` when lower; `NOT` is the dense/materializing case. | Implemented, including mixed-source alias coverage, native-destination limit guards, and oversized `NOT` coverage. | [#30](https://github.com/aviggiano/redis/issues/30), [#31](https://github.com/aviggiano/redis/issues/31), [PR #36](https://github.com/aviggiano/redis/pull/36), [PR #37](https://github.com/aviggiano/redis/pull/37) |
| Generic string command boundary | Pending clarification for final audit scope and whether `BITMAP CONVERT ... STRING` is the only v1 materialization escape hatch. | Draft behavior is implemented: native bitmaps reject generic string commands with `WRONGTYPE`; explicit conversion back to string is supported while materialization fits `proto-max-bulk-len`. | [#22](https://github.com/aviggiano/redis/issues/22), [PR #8](https://github.com/aviggiano/redis/pull/8), [PR #39](https://github.com/aviggiano/redis/pull/39) |
| Persistence, `DUMP` / `RESTORE`, and AOF rewrite | Settled for the current V2 direction: persist endian-neutral portable payloads, not host-endian CRoaring internals, and keep RESTORE-based rewrite for native bitmap values. | Implemented with `RDB_TYPE_BITMAP`, a stable v2 marker plus portable payload encoding, `DUMP` / `RESTORE`, AOF rewrite as `RESTORE`, always-on payload length/range validation for accepted legacy raw/range payloads, and a fallback for the previous same-version byte-length-first layout. | [#29](https://github.com/aviggiano/redis/issues/29), [PR #4](https://github.com/aviggiano/redis/pull/4), [PR #43](https://github.com/aviggiano/redis/pull/43), related open [PR #44](https://github.com/aviggiano/redis/pull/44) |
| Replication determinism | Ready locally: explicit `RESTORE`/`DUMP` propagation is the v1 deterministic baseline; sparse/container-specific propagation remains future work; replicas must be upgraded before native bitmap creation or conversion is enabled. | Implemented. Config-dependent native creations and conversions queue `RESTORE ... REPLACE [ABSTTL]`, native bitmap AOF rewrite emits `RESTORE`, and tests cover incremental AOF, rewrite, and replication convergence across mismatched local config. | [#28](https://github.com/aviggiano/redis/issues/28), [PR #9](https://github.com/aviggiano/redis/pull/9), [PR #13](https://github.com/aviggiano/redis/pull/13) |
| Introspection, copy, modules, and notifications | Module/keyspace-notification details are still pending final clarification. | `TYPE`, `SCAN ... TYPE`, `COPY`, `RedisModule_KeyType()`, `NOTIFY_BITMAP`, and type-change notifications exist in the draft implementation. | [#33](https://github.com/aviggiano/redis/issues/33), [PR #4](https://github.com/aviggiano/redis/pull/4), [PR #43](https://github.com/aviggiano/redis/pull/43) |
| Memory accounting, active defrag, and fork-child dismissal | Pending clarification around accounting policy, allocator hooks, and defrag acceptability. | Implemented draft lifecycle coverage exists, with follow-up review cleanup tracked separately. | [#32](https://github.com/aviggiano/redis/issues/32), [PR #18](https://github.com/aviggiano/redis/pull/18), [PR #43](https://github.com/aviggiano/redis/pull/43), [#45](https://github.com/aviggiano/redis/issues/45) |
| Test and benchmark gate | Pending clarification for required benchmark scope and acceptable event-loop stall bounds. | Correctness coverage exists across native type, command, oracle, corruption, replication, AOF, and module tests. Redis-specific benchmark harness remains open. | [#35](https://github.com/aviggiano/redis/issues/35), [#46](https://github.com/aviggiano/redis/issues/46), [#47](https://github.com/aviggiano/redis/issues/47) |

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
surface. `R64.*` coverage is limited to offsets that Redis bitmap commands can
parse and native Redis bitmaps can represent; for v1 that native range ends at
bit offset `4294967295`. Tooling must reject or explicitly policy-handle
redis-roaring `uint64_t` offsets outside that Redis-representable range. The
standalone `R.DIFF` / `R64.DIFF` command
names are therefore compatibility wrappers around behavior covered by
`BITOP DIFF` and remain out of v1 Redis scope.

| redis-roaring-only family | Gap versus Redis bitmap commands | Classification | v1 migration/import note |
| --- | --- | --- | --- |
| `R.SETINTARRAY`, `R.GETINTARRAY`, `R.RANGEINTARRAY`, `R.APPENDINTARRAY`, `R.DELETEINTARRAY`, and `R64.*` equivalents | Treat set bits as sorted integer arrays, including range paging and append/delete mutations. | Migration-tool-only | Required. Export should stream set offsets from `reroaring` and `roaring64` keys; import should build native bitmaps only when every 32-bit or 64-bit integer-array offset fits the v1 native cap. `R.RANGEINTARRAY` / `R64.RANGEINTARRAY` are useful for paged command-based export. |
| `R.SETBITARRAY`, `R.GETBITARRAY`, and `R64.*` equivalents | Use ASCII `0`/`1` bit-array strings rather than Redis raw bitmap strings. | Migration-tool-only | Optional compatibility format for tooling. Import can translate ASCII bit arrays to set offsets or native payloads; Redis core should prefer raw strings plus `BITMAP CONVERT` where a string representation fits. |
| `R.SETRANGE`, `R.SETFULL`, and `R64.*` equivalents | Create dense ranges or full universes of set bits. | Not needed for v1 | State migration can export the final set bits; command replay, if a tool supports it, can translate ranges outside Redis core. |
| `R.GETBITS`, `R.CLEARBITS`, and `R64.*` equivalents | Bulk `GETBIT`, bulk clear-to-zero, and an optional cleared-count reply. | Future Redis command candidate | Useful as generic batch bitmap operations, but not required to migrate stored values. Replay tooling can lower `CLEARBITS` to repeated clears or direct payload edits. |
| `R.CLEAR` and `R64.CLEAR` | Clear a module key without deleting the key name. | Not needed for v1 | Use `DEL` or an empty native bitmap during migration; no compatibility command is needed. |
| `R.MIN`, `R.MAX`, and `R64.*` equivalents | Return the first or last set integer. | Future Redis command candidate | `BITPOS key 1` covers the minimum; a reverse set-bit lookup could be considered later for maximum. Not required for import/export. |
| `R.CONTAINS`, `R.JACCARD`, and `R64.*` equivalents | Set-relation and similarity queries over two bitmaps. | Future Redis command candidate | Can be emulated for validation with `BITOP` plus `BITCOUNT` and temporary keys. Not required for state migration. |
| `R.OPTIMIZE` and `R64.OPTIMIZE` | Force CRoaring container optimization. | Not needed for v1 | Native Redis owns encoding optimization; migration tooling may optimize generated payloads internally. |
| `R.STAT` | Return module/container statistics for `reroaring` and `roaring64` keys. | Not needed for v1 | Use `MEMORY USAGE`, `OBJECT ENCODING`, benchmark tooling, or debug-only inspection for native stats if needed. |
| Module payloads: `reroaring`, `roaring64` | Module RDB / `DUMP` payloads are not native Redis bitmap payloads. | Migration-tool-only | Required. Tooling must read both module type names: `reroaring` uses CRoaring 32-bit serialized payloads and should be promoted to native `roaring64_bitmap_t`; `roaring64` uses CRoaring 64-bit portable payloads and can be validated/transcoded to the native bitmap RDB / `DUMP` representation when all set offsets fit the v1 native cap. Values beyond native bitmap offset limits must fail migration or follow an explicit tool policy. Redis core must not load these module payloads directly. |

## Native Bitmap Exposure Gate

The original gate prevented public `OBJ_BITMAP` creation until the branch had
enough type safety for Redis to own native bitmap keys everywhere they may
flow. The current draft implementation has crossed that implementation gate and
does expose native creation through `bitmap-default-roaring yes` and explicit
`BITMAP CONVERT`, but the design gate remains open for the pending tracker
decisions above.

Required before treating the current surface as final:

- RDB, AOF rewrite, `DUMP`, `RESTORE`, and replication support for native bitmap
  values.
- `TYPE`, `SCAN ... TYPE`, `COPY`, and `RedisModule_KeyType()` support for the
  new type.
- Free, copy, memory usage, and active defrag handling for bitmap objects.
  Memory usage means Redis live allocator-accounted key/object/value memory;
  benchmark comparisons to `redis-roaring` module `MEMORY USAGE` must account
  for that module's serialized-size callback and must not reduce native
  accounting correctness to make the rows match.
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
materialization or conversion). The correctness side has draft coverage; the
Redis-specific benchmark harness remains open in
[#47](https://github.com/aviggiano/redis/issues/47).

## Replication and AOF Determinism

DD-09 is resolved for v1 around an explicit serialized-payload baseline:
replicas and AOF replay must receive the representation chosen by the master,
not re-run native bitmap creation or conversion policy locally.

- A bitmap write that creates or converts a native bitmap because of
  `bitmap-default-roaring yes` queues `RESTORE <key> <ttl> <DUMP payload>
  REPLACE [ABSTTL]` for AOF and replication, and suppresses propagation of the
  triggering command.
- `BITMAP CONVERT` in either direction queues the same RESTORE form, preserving
  TTL with `ABSTTL` when needed.
- `BITOP` results whose native destination is chosen from local
  `bitmap-default-roaring yes` also propagate the final serialized value.
  Results that are native because an input key is already native can propagate
  as `BITOP`, since the source type is replicated logical state.
- AOF rewrite emits native bitmap values as `RESTORE <key> 0 <DUMP payload>
  REPLACE`, with key metadata inside the payload and expiration emitted through
  the normal AOF expiration path.
- The tradeoff is amplification: a small write that crosses the string/native
  boundary can append or replicate a full DUMP payload. Container-oriented or
  sparse transition commands can be evaluated later, but they are not required
  for v1 correctness.
- Version-skew policy is upgrade-first. Replicas and AOF replay targets must
  understand the native bitmap RDB/DUMP payload before operators enable native
  bitmap creation or conversion on a master.

## Creation and Conversion Surface

The current draft surface is a single `bitmap-default-roaring yes|no` flag,
defaulting to `no`, plus `BITMAP CONVERT <key> [NATIVE|STRING]`.

- With `bitmap-default-roaring no`, bitmap writes keep creating strings unless
  conversion is explicit.
- With `bitmap-default-roaring yes`, bitmap write commands create missing keys
  as native Roaring bitmaps and convert existing string values before writing.
- `BITMAP CONVERT <key> NATIVE` converts a legacy string bitmap to `bitmap`.
- `BITMAP CONVERT <key> STRING` converts back while the logical byte length can
  be materialized within `proto-max-bulk-len`.
- The size/saving thresholds and trial encodes from the early sketch are not in
  the current draft.

The implementation exists, but the final v1 command/config shape is still
tracked as pending clarification in [#20](https://github.com/aviggiano/redis/issues/20),
[#22](https://github.com/aviggiano/redis/issues/22), and
[#26](https://github.com/aviggiano/redis/issues/26).

## Oracle Test Strategy

Step 1 added a legacy-string oracle harness under `tests/support`. Later work
extended it with a `native-roaring` mode backed by `bitmap-default-roaring yes`,
so scenarios now compare bitmap-observable command results across both
representations. [PR #41](https://github.com/aviggiano/redis/pull/41) added
dedicated native bitmap oracle parity tests.

The initial corpus focuses on cases redis-roaring-style implementations commonly
stress:

- Sparse sets with large offsets.
- Byte, word, and Roaring-container boundary offsets.
- Repeated deterministic fuzz writes followed by `GETBIT` and `BITCOUNT`.
- String command boundary checks for values that are still legacy Redis strings.

## Settled v1 Decisions

- **Index width and public cap**: v1 keeps `roaring64_bitmap_t` internally and
  bounds native bitmap logical length to 512 MiB / max bit `4294967295`.
  Bitmap writes and dense/materializing paths also preserve the Redis bitmap
  safety limit when `proto-max-bulk-len` is lower; read-only native lookups can
  exceed that lower runtime limit only inside the native cap. Lifting the
  native cap is future work, not required for v1. See
  [#24](https://github.com/aviggiano/redis/issues/24) and
  [#42](https://github.com/aviggiano/redis/issues/42).

## Pending Decisions
- **Conversion/public surface**: confirm whether `BITMAP CONVERT` is needed in
  v1, whether it is the only new user-facing bitmap command, and whether
  `BITMAP CONVERT ... STRING` is sufficient as the materialization escape
  hatch. See [#20](https://github.com/aviggiano/redis/issues/20),
  [#22](https://github.com/aviggiano/redis/issues/22), and
  [#26](https://github.com/aviggiano/redis/issues/26).
- **Persistence format and AOF rewrite**: confirm whether the current
  `RDB_TYPE_BITMAP` portable v2 payload layout and RESTORE-based AOF rewrite
  are the final upstream shape for this core type. The current implementation
  stores a stable v2 marker plus endian-neutral portable payload, while the
  loader still accepts legacy raw/range payloads and the previous
  byte-length-first layout; open
  [PR #44](https://github.com/aviggiano/redis/pull/44) is related review
  follow-up, not current `unstable` behavior. See
  [#29](https://github.com/aviggiano/redis/issues/29).
- **Modules, notifications, and metadata**: settle event names/order, module
  observations, unlink callbacks, and metadata preservation policy. See
  [#33](https://github.com/aviggiano/redis/issues/33).
- **Memory management and defrag policy**: close the accounting, allocator-hook,
  active-defrag, and fork-child dismissal questions for v1. See
  [#32](https://github.com/aviggiano/redis/issues/32).
- **Benchmark and exposure gate**: define the Redis-specific benchmark harness,
  required scenarios, and acceptable event-loop stall bounds before treating
  the default/creation policy as final. See
  [#35](https://github.com/aviggiano/redis/issues/35) and
  [#47](https://github.com/aviggiano/redis/issues/47).
