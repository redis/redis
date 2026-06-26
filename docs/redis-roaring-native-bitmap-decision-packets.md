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
- The current RDB path saves a native bitmap with a stable v2 marker followed
  by an endian-neutral portable payload. Load accepts current portable payloads
  and older raw/range payloads above the server's current lower
  `proto-max-bulk-len` setting, but rejects native bitmap logical lengths above
  the 512 MiB v1 cap, max offsets above bit `4294967295`, and non-canonical
  legacy range payloads with overlapping or adjacent ranges.
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
- Bounded 32-bit v1 makes 32-bit redis-roaring inputs direct, but `roaring64`
  inputs must be rejected, down-converted only when safe, or deferred until a
  future wide native bitmap format exists.
- If operators need rollback from native bitmap to string, either option is
  bounded by `proto-max-bulk-len` because `BITMAP CONVERT key STRING` requires
  materialization to a Redis string.

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

- The current branch implements `BITMAP CONVERT <key> [NATIVE|STRING]`.
  Omitting the target defaults to `NATIVE`.
- Converting to the current representation is a no-op that replies `OK`.
- Missing keys return no-key error and non-string/non-bitmap values return
  `WRONGTYPE`.
- Conversion preserves TTL through `SETKEY_KEEPTTL`.
- `NATIVE` conversion changes `TYPE` from `string` to `bitmap`; `STRING`
  conversion changes it back to `string` only while the logical native length
  can be materialized within `proto-max-bulk-len`.
- The command is not propagated verbatim. The current branch queues a
  `RESTORE ... REPLACE [ABSTTL]` payload for the resulting value so replicas
  and AOF replay do not re-derive representation decisions from local config.
- With `bitmap-default-roaring no`, this command is the only explicit per-key
  opt-in path to native bitmaps. With `bitmap-default-roaring yes`, bitmap
  writes can auto-convert strings they touch.
- Explicit `BITMAP CONVERT` currently emits the shared overwrite/type-change
  events from `setKey()`, but no separate `bitmap convert` command event.

### Use Cases

- Per-key opt-in while the global default remains legacy string bitmap
  creation.
- Per-key rollback to string before disabling native bitmap support or before
  downgrade, when the value can still fit in a Redis string.
- Migration tooling that wants an explicit, deterministic Redis-native type
  transition without requiring a dummy bitmap write.
- Test and operational workflows that need to move a key between
  representations without changing all bitmap writes globally.

### Alternatives

| Option | Benefits | Costs / Risks |
| --- | --- | --- |
| Keep public `BITMAP CONVERT` in v1 | Clear per-key opt-in and rollback; keeps `bitmap-default-roaring no` useful; gives migration scripts a stable primitive. | Adds a new public command to document and support; event/metadata semantics must be settled before release. |
| Remove public conversion and rely on auto-conversion | Smaller command surface; answers the upstream concern that users may not need a separate command when auto-conversion exists. | No per-key opt-in under the legacy-preserving default; no native-to-string rollback command; migration scripts need another primitive; users may have to perform dummy writes to trigger conversion. |
| Keep conversion as hidden/debug/admin-only for v1 | Preserves internal test/migration utility while avoiding an early public API. | Does not solve user rollback; risks depending on unsupported surface; still needs propagation, persistence, and metadata semantics. |

### Compatibility Impact

- Keeping the command is additive, but once documented it becomes an API that
  clients, scripts, and migration tooling can depend on.
- Removing it before external release has low compatibility cost for Redis
  users but requires updating current branch tests, docs, and any migration
  plan that assumes explicit conversion.
- Removing it after release would be a breaking change unless an equivalent
  per-key native/string conversion path exists.

### Test Impact

- Keeping the command requires focused tests for round-trip conversion, TTL
  preservation, missing/wrong-type/no-op behavior, notification order, AOF and
  replication propagation, and failure to convert oversized native values back
  to string.
- Removing the command requires deleting or rewriting current conversion tests
  and adding replacement coverage for whatever migration or rollback mechanism
  remains.
- Any option still needs tests that conversion does not depend on replica
  `bitmap-default-roaring` settings or local `proto-max-bulk-len` settings.

### Migration Impact

- Keeping `BITMAP CONVERT` gives migration tools a simple local step: import or
  create a logical string bitmap, convert selected keys to native, validate,
  and convert back to string for rollback when possible.
- Without it, migration either enables `bitmap-default-roaring yes` globally,
  uses RESTORE/DUMP payloads directly, or needs a separate migration-only
  command/tool. Each alternative is harder to explain and harder to roll back
  per key.
- Rollback is inherently limited once a native bitmap cannot be materialized
  within `proto-max-bulk-len`; the command should document that limit if kept.

### Recommendation

Keep `BITMAP CONVERT` in v1 if native bitmaps remain an observable type and the
default remains `bitmap-default-roaring no`. The command is the cleanest
per-key opt-in and rollback primitive.

If maintainers decide to remove it, require an explicit replacement for both
per-key migration and native-to-string rollback before changing the branch.

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
- Current explicit `BITMAP CONVERT` uses `setKey()`, so subscribers see
  `overwritten` and `type_changed`, but no separate command event.
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
| Command event | Explicit `BITMAP CONVERT` emits only transition events | Current behavior; concise event stream. | No command-specific event for audit subscribers. |
| Command event | Explicit `BITMAP CONVERT` emits transition plus bitmap `convert` event | Gives subscribers a command-shaped audit event. | Adds a new event name and ordering question. |
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
  explicit conversion and auto-conversion in both directions where supported.
- If explicit conversion emits a command event, add Pub/Sub and module callback
  tests for the event name and order.
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
explicitly before exposing the command.

For explicit `BITMAP CONVERT`, consider adding a bitmap-class `convert` event
after the type transition so audit subscribers can distinguish an intentional
conversion from a generic overwrite.
