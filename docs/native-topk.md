# Native Top-K

This PR targets `feature/native-probabilistic` independently; see
[the development workflow](probabilistic-development.md).

All seven TOPK commands live in `src/probabilistic/t_topk.c`, with the reusable HeavyKeeper
algorithm in `src/probabilistic/topk.c`. The opt-in CuckooHeavyKeeper engine reuses
`src/chk.c`, also used by core hot-key tracking. Its key lifecycle and persistence
adapter lives in `src/probabilistic/topk_chk.c`; no second CHK implementation is added.
No additional dependency or module loading is required. The temporary
`BUILD_BLOOM` switch enables the migrated families present in this checkout.

Native objects support COPY, expiry, UNLINK, memory accounting, digest and
defragmentation. `TYPE` returns `topk`; the ACL category is `@topk`;
notification character `k` is included in `A`.

## Opt-in CuckooHeavyKeeper

The default remains `heavykeeper`. To select CHK for subsequent reservations:

```text
CONFIG SET topk-default-algorithm cuckoo-heavykeeper
TOPK.RESERVE new-key 100
TOPK.INFO new-key
```

The setting also works in redis.conf or as `--topk-default-algorithm`; use
`CONFIG REWRITE` to persist a runtime change. It affects only new keys, never
existing keys, COPY, RESTORE, reloads or replication. Switching the setting back
to `heavykeeper` does not convert CHK keys. There is no in-place conversion.

CHK reservations accept only `TOPK.RESERVE key k`. Each of its two tables has
`8*k` buckets rounded up to the next power of two, and decay is fixed at `1.08`.
The supported k range is 1..134217728, subject to address space and available
memory. Legacy `width depth decay` arguments are rejected in CHK mode, not
reinterpreted. Tuning CHK parameters separately is outside this initial opt-in.

CHK `TOPK.INFO` returns `k`, `buckets` (per table), `decay` and
`algorithm=cuckoo-heavykeeper`. Legacy INFO retains its original
`k/width/depth/decay` response for compatibility. Both engines report TYPE `topk`.
QUERY tests heap membership; COUNT estimates frequency from the sketch, including
items outside the heap. CHK zero increments are no-ops; unlike legacy behavior,
they do not insert a zero-count item. INCRBY retains the per-item 0..100000 limit.
CHK totals are limited to `LLONG_MAX` so counts fit RESP integers; overflowing
increments return per-item errors without changing that item, while valid
increments in the same command still apply.

There is no measured throughput/accuracy equivalence guarantee. Benchmark at
equal accuracy or memory budgets before enabling. On a 64-bit build, CHK's
algorithm storage is approximately `6232 + 80*buckets + 24*k` bytes plus item
SDS allocations, allocator overhead and Redis metadata. The native engine tag
adds one small wrapper allocation to either engine. CHK is not a security upgrade.

## Upgrade and existing keys

The existing `TopK-TYPE` version-0 wire format is retained. Existing keys
load without application recreation. Real restart-upgrade tests cover RDB,
plain rewritten AOF and RDB-preamble AOF, including two databases, empty keys,
absolute expiry and continued writes. See [the family upgrade procedure](probabilistic-upgrade.md).

The legacy format stores a native-layout heap blob, so this is a same-ABI
upgrade path, not a new cross-architecture conversion format. Native saving
clears process pointers and padding and preserves embedded-NUL item lengths;
bytes already truncated by an old module snapshot cannot be reconstructed.
The legacy 32-bit MurmurHash2 word read uses `memcpy` to avoid unaligned-load
undefined behavior; this preserves the original native-endian hash bytes.

CHK uses `TopK-TYPE` encoding version **1**, with an explicit engine tag:
algorithm, k, buckets/table, decay, total, packed sketch, packed heap metadata,
occupied item strings, EOF. Integers in packed records are little endian;
fingerprints and 64-bit counters have fixed widths. No pointers, C padding,
allocator accounting or derived lookup tables are saved. Heap order and all
sketch counters are retained, not reconstructed from TOPK.LIST.

Loaders reject unknown versions/engines, invalid dimensions, malformed lengths,
unsupported decay, invalid counts/heap order, duplicate names and bad item
fingerprints. Version 0 always loads HeavyKeeper; version 1 always loads CHK,
independent of configuration. Historical untagged TOPK.RESERVE commands in an
old AOF or replication stream retain their original HeavyKeeper meaning.
Old binaries/modules cannot read CHK keys; this is an upgrade-only feature.
Upgrade all replicas and migration targets before enabling it.

## Replication tradeoff

Reservations (including empty keys) and randomized updates propagate exact
object state via RESTORE, retaining the
absolute expiry. This prevents replicas and AOF replay from rerunning random
decay. It currently costs O(sketch size) per write; a compact deterministic
delta is needed before this draft is production-ready for large sketches. CHK
does not remove this cost; the default sizing can produce substantially larger
payloads than legacy default sketches. Replicas need not share the primary's
default setting to preserve existing keys, but defaults should match for future
client reservations after promotion.

## Tests

`make test-topk` runs assertion-based algorithm workloads, all migrated
upstream flow cases, corrupt-RDB protection and native lifecycle/RESP3/ACL,
replication and AOF regressions. CHK cases cover both engines together, config
switching, exact RDB/DUMP/AOF round trips, different replica defaults/full resync,
historical command AOFs, binary names, 64-bit counts/overflow and corrupt payloads.
With `-DREDIS_TEST`, the target also runs core CHK algorithm units.
It is included by `make test`, event CI and
the Ubuntu nightly. Optional external compatibility/upgrade tests use
`BLOOM_ORACLE_SERVER` and `BLOOM_ORACLE_MODULE`. Tests require only Python 3.
