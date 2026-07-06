# Native Bitmap Decision Packets

This document prepares local review packets for the native bitmap design
questions tracked by aviggiano/redis#48. It is intentionally decision support,
not an implementation change and not an external-facing conclusion.

The packets below separate facts from recommendations. "Current implementation"
describes the branch state at the time this document was written; it does not
settle the design.

Final v1 outcome for Packet 1: native bitmaps keep `roaring64_bitmap_t`
internals, but native bitmap values remain capped at 512 MiB of logical bytes
(max bit offset `4294967295`). Redis bitmap writes remain bounded by
`proto-max-bulk-len` for allocation safety, and read-only native bitmap lookups
may only exceed a lower current `proto-max-bulk-len` setting while staying
inside the native v1 cap.

Primary local references:

- `docs/redis-roaring-native-bitmap-design.md`
- `docs/redis-roaring-pr-breakdown.md`
- aviggiano/redis#24, aviggiano/redis#26, aviggiano/redis#33
- upstream discussion links recorded in those issues, especially
  redis/redis#15296 and redis/redis#15331 review comments

Useful source links:

- Index width follow-up: https://github.com/aviggiano/redis/issues/24
- Conversion controls follow-up: https://github.com/aviggiano/redis/issues/26
- Notification/module follow-up: https://github.com/aviggiano/redis/issues/33
- Upstream design summary: https://github.com/redis/redis/issues/15296#issuecomment-4718567387
- Allocation concern: https://github.com/redis/redis/issues/15296#issuecomment-4718795474
- `BITMAP CONVERT` question: https://github.com/redis/redis/issues/15296#issuecomment-4725438966
- Notification and metadata comment: https://github.com/redis/redis/issues/15296#issuecomment-4727376355
- PR review on high offsets: https://github.com/redis/redis/pull/15331#discussion_r3413389841
- PR review on `NOTIFY_BITMAP`: https://github.com/redis/redis/pull/15331#discussion_r3412619066

## Packet 1: v1 Index Width

### Decision Needed

Resolved by DD-17: v1 keeps the current `roaring64_bitmap_t` internal
representation with a bounded native bitmap surface instead of switching the
storage implementation to bounded 32-bit Roaring before the format is treated
as stable.

### Facts and Current Implementation State

- Native bitmap objects are already distinct Redis values (`OBJ_BITMAP`) with
  `TYPE` returning `bitmap`.
- The current code stores native bitmaps with CRoaring's
  `roaring64_bitmap_t` and a separate `uint64_t byte_len`.
- Public write paths reject offsets that address a byte at or beyond
  `proto-max-bulk-len` for normal clients. Native bitmap values additionally
  reject offsets above bit `4294967295`; read-only native lookups return zero
  for unset bits beyond the logical length only within that v1 range.
- Dense/materializing native paths remain bounded by `proto-max-bulk-len` when
  that configured limit is lower; read-only native lookups can exceed a lower
  runtime limit only inside the 512 MiB v1 cap.
- The current RDB path saves a native bitmap as Redis-owned observable bitmap
  bytes: `byte_len` followed by a Redis raw string with the same decoded length.
  Because this PR has not shipped, load accepts only that final raw payload
  rather than previous in-PR draft raw/range/container/portable payloads. It
  rejects native bitmap logical lengths above the 512 MiB v1 cap and malformed
  raw string payloads.
- Documentation now describes native 64-bit-capable internals with the bounded
  v1 native bitmap surface, leaving any future cap expansion as a separate
  decision.
- The upstream concern that reopened this question is allocation safety:
  extremely high offsets can still lead to expensive dense operations, digest
  paths, materialization, or future feature pressure if the type advertises a
  wider index space than v1 actually supports.

### Options

| Option | Benefits | Costs / Risks |
| --- | --- | --- |
| Keep 64-bit Roaring with the bounded v1 cap | Minimizes current code churn; keeps 64-bit-capable storage internals for future native-offset work; maps naturally from redis-roaring `roaring64` migration inputs after validation; permits sparse read lookups on native values inside the v1 cap. | Carries 64-bit internal overhead while v1 remains bounded; keeps future pressure to lift caps; safety review must prove dense/materializing paths respect the bounded surface. |
| Switch v1 to bounded 32-bit Roaring | Aligns storage width with the current effective v1 cap; avoids advertising unused 64-bit behavior; likely smaller/faster internal directory for bounded data; makes allocation-safety story simpler. | Requires implementation churn; makes later 64-bit support a new format/type migration; cannot represent redis-roaring `roaring64` inputs above the 32-bit range; raised `proto-max-bulk-len` deployments would need an explicit native bitmap cap. |
| Ship dual 32-bit and 64-bit native formats in v1 | Can optimize bounded keys while preserving future wide-key support. | Adds format, RDB, command, migration, and test complexity before v1 semantics are settled. This is the least concise v1 story. |

### Compatibility Impact

- Legacy string bitmap behavior remains bounded by `proto-max-bulk-len` in all
  options.
- Keeping 64-bit internally with a bounded native surface is compatible with
  the current branch behavior; docs must not promise native reads or writes
  beyond the v1 native cap.
- Switching to bounded 32-bit is user-visible only if native bitmaps are
  expected to support raised `proto-max-bulk-len` values above the 32-bit
  bitmap range, or if migration tools encounter `roaring64` inputs with set
  bits above that range.
- Any shipped RDB type that later needs a wider index model should be treated
  as a compatibility boundary. If v1 ships bounded 32-bit, future 64-bit native
  bitmaps should use an explicit format/version migration rather than silently
  changing the meaning of existing payloads.

### Test Impact

- Keeping 64-bit requires tests that prove every public write path, read path,
  `DUMP`/`RESTORE`, AOF rewrite, replication transition, `DEBUG DIGEST`, and
  conversion/materialization path respects the selected cap.
- Switching to 32-bit requires updating boundary tests around the exact native
  max bit, raised `proto-max-bulk-len` behavior, RDB corrupt-payload handling,
  and mixed string/native command parity.
- Both options still need regression coverage for `BITOP NOT` and other dense
  result paths, because the safety risk is about memory growth as much as index
  width.

### Migration Impact

- Keeping 64-bit makes redis-roaring `roaring64` migration conceptually direct,
  while 32-bit redis-roaring inputs can be widened.
- The [#34 final adjudication](https://github.com/aviggiano/redis/issues/34#issuecomment-4832039784)
  resolves v1 migration as an external streaming migrator. That tool must
  enforce the issue-level native cap, default above-cap `roaring64` inputs to
  `fail`, and avoid silent truncation even though the in-memory representation
  uses `roaring64_bitmap_t`.
- Bounded 32-bit v1 makes 32-bit redis-roaring inputs direct, but `roaring64`
  inputs must be rejected, down-converted only when safe, or deferred until a
  future wide native bitmap format exists.
- If operators need rollback from native bitmap to string, v1 requires an
  external string representation or an ordinary overwrite such as `SET`;
  Redis core does not expose native-to-string materialization.

### Resolution

Final v1 direction keeps 64-bit-capable Roaring internals while bounding native
bitmap logical length to 512 MiB / max bit `4294967295`. This preserves
redis-roaring `roaring64` migration headroom for values that validate inside
the v1 range and avoids reopening the storage format, while keeping the
allocation-safety boundary reviewers asked for. Normal writes and
dense/materializing paths still honor `proto-max-bulk-len` when it is lower,
while read-only native lookups can exceed that lower runtime limit only inside
the v1 cap. Lifting the native cap remains a future compatibility and
performance decision.

## Packet 2: `BITMAP CONVERT` v1

### Decision Needed

Does v1 need `BITMAP CONVERT <key> [NATIVE|STRING]` as a public command, or
should conversion happen only through `bitmap-default-roaring yes` writes and
migration tooling?

### Facts and Current Implementation State

- The v1 branch no longer exposes `BITMAP CONVERT`.
- With `bitmap-default-roaring no`, bitmap writes preserve legacy string
  creation unless a key is already native through load/RESTORE/replication or a
  native BITOP source.
- With `bitmap-default-roaring yes`, bitmap writes create missing keys as
  native bitmaps and convert existing strings before writing.
- Config-dependent type transitions are not propagated verbatim. The branch
  queues `RESTORE ... REPLACE [ABSTTL]` payloads for the resulting value so
  replicas and AOF replay do not re-derive representation decisions from local
  config.

### Use Cases

- Preserve Redis' legacy string bitmap creation behavior by default.
- Keep threshold-based conversion configs and trial encodes out of v1.
- Avoid adding a public conversion API before the module, event, rollback, and
  migration semantics are clearly needed by users.
- Keep native creation available through `bitmap-default-roaring yes` and
  native load/RESTORE paths.

### Alternatives

| Option | Benefits | Costs / Risks |
| --- | --- | --- |
| Keep public `BITMAP CONVERT` in v1 | Clear per-key opt-in and rollback; gives migration scripts a stable primitive. | Adds a new public command to document and support; event/metadata semantics must be settled before release. |
| Remove public conversion and rely on configured native writes / RESTORE | Smaller command surface; answers the upstream concern that users may not need a separate command; keeps v1 focused on existing bitmap commands. | No per-key opt-in under the legacy-preserving default; no native-to-string rollback command; migration scripts must use RESTORE payloads or another external primitive. |
| Keep conversion as hidden/debug/admin-only for v1 | Preserves internal test/migration utility while avoiding an early public API. | Does not solve user rollback; risks depending on unsupported surface; still needs propagation, persistence, and metadata semantics. |

### Compatibility Impact

- Removing it before external release has low compatibility cost for Redis
  users and avoids committing to an API whose use cases are still unclear.
- Adding it after v1 remains possible if migration or operations need a public
  per-key conversion primitive.

### Test Impact

- Tests should assert that `BITMAP`/`BITMAP CONVERT` are not part of the v1
  command table.
- Existing native fixture tests should create native values through
  `bitmap-default-roaring yes` writes or through native RESTORE payloads.
- Config-dependent type transitions still need AOF/replication coverage that
  proves replicas do not re-run local `bitmap-default-roaring` decisions.

### Migration Impact

- Migration tooling should use validated target-native RESTORE payloads or the
  external migrator contract, not a Redis core public conversion command.
- Rollback can still replace a key with a string via ordinary writes when the
  caller has a string representation, but v1 does not promise native-to-string
  materialization.

### Recommendation

Do not keep `BITMAP CONVERT` in v1. Preserve legacy string bitmap creation by
default, keep threshold configs out, and limit native creation to
`bitmap-default-roaring yes` writes plus native load/RESTORE paths.

## Packet 3: Module and Keyspace Notification Semantics

### Decision Needed

Which notification classes, event order, module observations, and key metadata
rules should native bitmap creation and conversion use?

### Facts and Current Implementation State

- The current branch adds `NOTIFY_BITMAP` / `REDISMODULE_NOTIFY_BITMAP` using
  the `b` notify-keyspace-events character, and includes bitmap events in
  `NOTIFY_ALL`.
- `RedisModule_KeyType()` currently returns `REDISMODULE_KEYTYPE_BITMAP` for
  native bitmap values.
- Native bitmap writes currently emit bitmap-class command events such as
  `setbit` and `set`.
- Current native key creation through `bitmap-default-roaring yes` emits `new`
  and then the bitmap command event.
- Current string-to-native conversion through a bitmap write uses `setKey()`,
  so subscribers see `overwritten`, then `type_changed`, then the trailing
  bitmap command event.
- Because conversion currently uses the overwrite path, module key-unlink
  callbacks run with overwrite flags and module key metadata is discarded
  except for expiration metadata.
- Module keyspace subscribers are invoked before Pub/Sub filtering by
  `notify-keyspace-events`, but only subscribers whose event mask matches the
  event type are called.

### Choices

| Choice | Option | Benefits | Costs / Risks |
| --- | --- | --- | --- |
| Event class | Keep `NOTIFY_BITMAP` | Lets modules and Pub/Sub distinguish bitmap writes from string writes after the type split; matches upstream reviewer direction. | Adds a new notify class and docs surface. |
| Event class | Reuse `NOTIFY_STRING` for bitmap commands | Minimizes notify classes. | Blurs the observable type split and makes module filtering less precise. |
| Conversion event shape | Treat conversion as overwrite plus type change | Reuses shared DB semantics; watchers and modules already understand replacement. | Implies metadata loss and key-unlink callbacks; may overstate how destructive representation conversion should be. |
| Conversion event shape | Treat conversion as type transition, preserving metadata | Matches the intuition that logical bitmap data is unchanged; avoids losing module metadata. | Requires a new DB transition path and clear module callback contract. |
| Ordering | Transition events before triggering command event | Current auto-conversion order; subscribers observing the command event see the new type. | Subscribers see multiple events for one command and may need documentation. |
| Ordering | Triggering command event before transition events | Preserves "command happened first" intuition. | Subscribers observing the command event may still see the old type, which is awkward for modules. |

### Compatibility Impact

- Keeping `NOTIFY_BITMAP` is an additive module/PubSub surface, but changing
  event class later would break subscribers that filter on `b`.
- Reclassifying native bitmap events as string events would be less disruptive
  for old notification consumers but inconsistent with `TYPE bitmap`.
- Keeping overwrite semantics is compatible with generic Redis overwrite event
  behavior, but it can break modules that attach per-key metadata and expect a
  representation conversion to preserve it.
- Preserving metadata during conversion would be friendlier to modules, but it
  requires defining whether key-unlink callbacks should run at all.

### Test Impact

- Current tests cover bitmap notification class routing and the current
  `new`/`overwritten`/`type_changed`/command-event order.
- If conversion preserves metadata, add tests that module metadata survives
  auto-conversion where supported.
- If key-unlink callbacks are skipped for conversion, add module tests proving
  overwrite callbacks still run for real overwrites and do not run for
  representation-only transitions.

### Migration Impact

- Metadata loss during conversion can force modules to rebuild secondary
  indexes or per-key state after migrating string bitmaps to native bitmaps.
- Metadata preservation makes staged migration safer for modules, especially
  when conversion is used as a representation change rather than a logical
  data replacement.
- A stable event order lets migration tooling and modules observe conversion
  progress without parsing command streams or relying on AOF/replication
  internals.

### Recommendation

Keep `NOTIFY_BITMAP` and keep transition events before the trailing bitmap
command event for auto-conversion.

Prefer preserving module key metadata during string/native bitmap conversion,
with a dedicated type-transition path rather than the generic overwrite path.
If that is too much for v1, document the overwrite semantics and metadata loss
explicitly for configured bitmap-write conversion.
