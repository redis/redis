# Native Top-K

All seven TOPK commands live in `src/t_topk.c`, with the reusable HeavyKeeper
algorithm in `src/topk.c`. This is separate from core hot-key tracking:
the two implementations can be evaluated independently by the data-types team.
No additional dependency or module loading is required. The temporary
`BUILD_BLOOM` switch enables the migrated family together.

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

## Replication tradeoff

Randomized updates propagate exact object state via RESTORE, retaining the
absolute expiry. This prevents replicas and AOF replay from rerunning random
decay. It currently costs O(sketch size) per write; a compact deterministic
delta is needed before this draft is production-ready for large sketches.

## Tests

`make test-topk` runs assertion-based algorithm workloads, all migrated
upstream flow cases, corrupt-RDB protection and native lifecycle/RESP3/ACL,
replication and AOF regressions. It is included by `make test`, event CI and
the Ubuntu nightly. Optional external compatibility/upgrade tests use
`BLOOM_ORACLE_SERVER` and `BLOOM_ORACLE_MODULE`. Tests require only Python 3.
