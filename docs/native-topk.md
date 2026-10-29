# Native Top-K

This PR targets `feature/native-probabilistic` independently; see
[the development workflow](probabilistic-development.md).

All seven public TOPK commands live in `src/probabilistic/t_topk.c`, with the reusable HeavyKeeper
algorithm in `src/probabilistic/topk.c`. This is separate from core hot-key tracking:
the two implementations can be evaluated independently by the data-types team.
No additional dependency or module loading is required. The temporary
`BUILD_BLOOM` switch enables the migrated families present in this checkout.

Native objects support COPY, expiry, UNLINK, memory accounting, digest and
defragmentation. `TYPE` returns `topk`; the ACL category is `@topk`;
notification character `k` is included in `A`.

## Upgrade and existing keys

The existing `TopK-TYPE` version-0 wire format is retained. Existing keys
load without application recreation. Real restart-upgrade tests cover RDB,
plain rewritten AOF and RDB-preamble AOF, including two databases, empty keys,
absolute expiry and continued writes. See [the family upgrade procedure](probabilistic-upgrade.md).

The legacy format stores a native-layout heap blob, so this is a same-ABI
upgrade path, not a new cross-architecture conversion format. Native saving
clears process pointers and padding and preserves embedded-NUL item lengths;
bytes already truncated by an old module snapshot cannot be reconstructed.

## Exact-state replication

Randomized updates propagate the final bucket in each row through the internal
`TOPK._APPLY` command. Replicas and incremental AOF replay apply those counters
and the deterministic heap update without rerunning random decay. Updates leave
the existing expiry untouched. Partial-success INCRBY commands record only the
successful items, in order; binary and empty items are preserved.

Updates only capture and encode these deltas when Redis's propagation gate has
an eligible destination. With AOF disabled and no replica, replication backlog
or slot migration, updates run without allocating replay buffers or rewriting
the command. Enabling AOF or replication later resumes exact delta propagation.

The versioned delta uses one byte for its version and, per item, four bytes for
its length, the item bytes and eight bytes per depth row. Its size is independent
of sketch width and heap size. Ordinary clients cannot execute this command.
RDB and rewritten AOF continue to use the existing full-state format. Replicas
and AOF readers must support the internal delta command before receiving writes
from this version; old module readers cannot replay incremental deltas.

## Tests

`make test-topk` runs assertion-based algorithm workloads, all migrated
upstream flow cases, corrupt-RDB protection and native lifecycle/RESP3/ACL,
replication and AOF regressions. It is included by `make test`, event CI and
the Ubuntu nightly. Optional external compatibility/upgrade tests use
`BLOOM_ORACLE_SERVER` and `BLOOM_ORACLE_MODULE`. Tests require only Python 3.
