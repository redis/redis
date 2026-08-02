# Point-in-time HASH snapshots (value-MVCC) — Design

Status: **implemented**
Scope: **HASH values only.** The snapshot read API rejects every other type.
Audience: Redis core + module (RediSearch/RedisJSON) teams

> Prefer the named functions when navigating the code; line numbers drift.

---

## 1. Motivation

A module such as RediSearch runs queries on its own worker threads against a
point-in-time snapshot of its *index*. To materialize results it must read the
actual field values of matching documents, which live in the core keyspace as
`HASH` values. Reading the **live** keyspace is racy: a document is selected on
its indexed state at time `T`, but its fields are read at `T+δ`, reflecting
writes the query should not see.

The naive fix — deep-copy every candidate value at query start — doubles RAM for
the working set. This feature gives a module a consistent, cheap, point-in-time
view of the HASH keys in a DB without copying values that were never touched.

## 2. Public API

Four module functions (`src/module.c`, declared in `src/redismodule.h`):

| Function | Purpose |
|---|---|
| `RM_CreateKeyspaceSnapshot(ctx)` | Open a snapshot of the caller's DB. Returns an opaque handle. |
| `RM_SnapshotOpenKey(ctx, snap, keyname)` | Open a key as-of the snapshot. **Returns NULL for any non-HASH value** — the read API is hash-only. The returned `RedisModuleKey` reads with the normal hash APIs (e.g. `RM_HashGet`). |
| `RM_SnapshotHashGet(ctx, snap, keyname, field)` | Cheap single-field read as-of the snapshot, without materializing the whole hash. Returns NULL if the field was absent as-of the snapshot. |
| `RM_FreeKeyspaceSnapshot(ctx, snap)` | Close the snapshot and trigger version reclamation. |

Snapshots are also reachable from a worker thread under the GIL
(`RM_ThreadSafeContextLock`), which is the intended query-worker usage.

Observability: `INFO` exposes `hash_snapshots_open`, `hash_snapshot_deltas`,
`hash_snapshot_preserved_bytes` (chains plus frozen hashes) and
`hash_snapshot_evicted_drops`. `DEBUG KVSNAPSHOT {create|free|hget|len|stats}`
drives the mechanism directly from tests without a module.

## 3. Mechanism (shared, version-stamped store)

A single global epoch counter, `gEpoch`, is bumped **only when a snapshot is
created** (not per write). A snapshot records the epoch at its creation as
`base_epoch`.

Before a hash field changes — `hashTypeSet`, `hashTypeDelete`, or hash-field TTL
expiry — its pre-image is recorded **once** into a shared, per-DB store:

```
gHStore[dbid]:  keyname(sds) -> fieldstore(dict)
fieldstore:     field(sds)   -> chain of { epoch, old-bytes }   (version-ordered)
```

`old == NULL` is a **tombstone**: the field did not exist before this epoch.

Key properties:

- **Recorded once, serves every open snapshot.** Regardless of how many
  snapshots are open, a given (key, field) mutation writes at most one record
  per epoch (first-touch-per-epoch dedup). Write cost is O(1) in the open-
  snapshot count — contrast a per-snapshot store, which is O(S).
- **Zero cost when idle.** Every capture site is gated on
  `server.snapshots_open`; with no snapshot open the only added work on the hot
  write path is one predicted-not-taken branch:
  ```c
  if (unlikely(server.snapshots_open)) snapshotHashCapture(db, o, field);
  ```
  That gate is global, so `snapshotHashCapture()` re-checks a per-DB open-snapshot
  count (`gSnapOpenDb`) inside the out-of-line function: a snapshot on one DB must
  not make another DB's writes record chains nothing can resolve.
- **Read resolves by epoch.** A read as-of a snapshot finds the first chain
  record with `epoch > base_epoch` (binary search). If none exists the field is
  unchanged since the snapshot and is read straight from the live hash.
- **Whole-key removal materializes.** On `DEL`/overwrite of a HASH key
  (`dbGenericDelete` / `dbSetValue`), the as-of-V hash is materialized into every
  in-scope open snapshot's `preserved` dict *before* the live value is freed, so
  untouched fields survive the free. Non-hash removals do nothing.
- **Flush materializes then clears.** `FLUSHDB`/`FLUSHALL` wipe the keyspace via
  `kvstoreEmpty`, bypassing the per-key hooks, so `emptyDbStructure` calls
  `snapshotHashPreserveOnFlush`: it materializes every hash key of the flushed DB
  into the in-scope snapshots and drops the DB's shared store, so a key recreated
  after the flush cannot inherit a stale delta chain.
- **Version-watermark reclamation.** When a snapshot is freed, the watermark is
  the oldest open snapshot's `base_epoch` (or `gEpoch` if none remain); chain
  records with `epoch <= watermark` are pruned. No per-record refcounts.

## 4. Read paths

- **Per-field** (`kvSnapshotHashField`, behind `RM_SnapshotHashGet`): resolve the
  field's chain, or fall through to the live field if unchanged. No whole-hash
  copy. This is the path a document loader should prefer.
- **Whole-value** (`kvSnapshotView`, behind `RM_SnapshotOpenKey`): copy the live
  hash and apply the snapshot's resolved deltas into a frozen `kvobj` cached in
  `preserved`, so subsequent opens are O(1). Materializing re-inflates memory for
  that key for the snapshot's lifetime — an explicit trade for a fully-formed
  hash object the standard key APIs can read.

## 5. Data structures (`src/kvsnapshot.c`)

```c
struct keyspaceSnapshot {          /* one open point-in-time view of a DB */
    uint64_t id;                   /* handle for the DEBUG command */
    int      dbid;
    uint64_t base_epoch;           /* reads resolve chain records epoch > base_epoch */
    dict    *preserved;            /* keyname -> frozen as-of-V hash kvobj */
};
typedef struct { uint64_t epoch; sds old; } snapHRec;   /* old==NULL => tombstone */
typedef struct { snapHRec *r; int n, cap; } snapHChain;
```

Statics: `gEpoch` (bumped per snapshot creation), `gHStore` (per-DB keystores,
lazily allocated), `gSnapOpenDb` (per-DB open-snapshot counts, the DB-scope gate),
`nextSnapshotId`, `inMaterialize` (re-entrancy guard so materialization does not
re-enter capture). Open snapshots are tracked in `server.keyspace_snapshots`.

## 6. Performance

- **Idle:** perf-neutral. The only hot-path addition is the gated branch above;
  measured SET/HSET throughput is within run-to-run noise of the base build.
- **Write with snapshots open:** O(1) in the number of open snapshots (one shared
  record per changed field per epoch), vs O(S) for a per-snapshot design.
- **Memory:** proportional to the number of *distinct changed fields* since the
  oldest open snapshot, not to the number of snapshots. Whole-value reads
  additionally hold a materialized copy per read key for the snapshot's lifetime.
  Both are counted in `hash_snapshot_preserved_bytes`.

## 7. Limitations / scope

- **HASH only.** `RM_SnapshotOpenKey` returns NULL for non-hash values by design;
  other types are out of scope for this change.
- No cross-field / cross-key version journal: the per-field index is sufficient
  for point reads, whole-hash materialization, and reclamation.
- `SWAPDB` retargets open snapshots (`snapshotOnSwapDb`) so they follow the
  keyspace they snapshotted to its new index. A diskless full-sync swap
  (`swapMainDbWithTempDb`) discards that keyspace instead, so it invalidates every
  open snapshot (`snapshotInvalidateAll`): reads return absent and the held memory
  is released at once, without waiting for the module to free the handle.
- A key **evicted** under `maxmemory` while a snapshot is open is dropped from the
  snapshot rather than preserved, since preserving it would copy the value the
  eviction is reclaiming. This holds whether or not the snapshot had already
  materialized the key — an existing frozen copy is dropped too. The drop is
  all-or-nothing (the pre-image chain goes with it, so a reader sees an absent key,
  never a partial hash) and is counted in `hash_snapshot_evicted_drops`. Expiry
  still preserves.
- TTLs (both hash-field HFE and whole-key) are frozen as-of-V: a field or key
  alive at snapshot time stays readable regardless of how much wall-clock time
  later passes. All read paths use `ACCESS_EXPIRED` for this — captured
  pre-images, the materialized hash, and the unchanged-field/key live fallback
  (the live key lookup also passes `NOEXPIRE` so a snapshot read never deletes a
  live key). The mechanism is version- (epoch-) based rather than wall-clock-
  based, so a field/key that had already expired *before* the snapshot but was
  not yet reaped is treated as still present as-of-V; this narrow edge is accepted.
- Reclamation is coarse (freed-snapshot-triggered watermark), which is adequate
  for the query-worker pattern of a bounded set of concurrently-open snapshots.
