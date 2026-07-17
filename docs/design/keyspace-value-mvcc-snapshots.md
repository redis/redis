# Keyspace Value-MVCC Snapshots — Design

Status: **Draft / accepted direction**
Scope: **this PR delivers the generic value-MVCC core.** Follow-up PRs (scope
controls; hash field delta-log) and the JSON path-delta contract are sketched in
§7 as a roadmap — each lands its own doc section alongside its code, so this
document grows incrementally with the feature.
Audience: Redis core + RediSearch/RedisJSON module teams

> Line numbers in this document are indicative; prefer the named functions when
> navigating, as line numbers drift.

---

## 1. Motivation

RediSearch runs queries on its own worker threads against its own point-in-time
snapshot of its **index**. To materialize results it must load the actual field
values of the matching documents, which live in the **core keyspace** as `HASH`
(core aggregate) and `JSON` (RedisJSON module type) values.

Today those loads read the **live** keyspace, which mutates concurrently and is
out of sync with the index snapshot the query is based on. The result is a
consistency gap: a document can be selected based on its indexed state at time
`T`, but its field values are read at some later time `T+δ`, reflecting writes
that happened in between (or a deletion). The query can return a document whose
loaded values no longer match the predicate it was selected by.

The goal is a **core keyspace snapshot primitive**, readable from a module's
worker thread, whose consistency point RediSearch can pin its index snapshot to.

### Consumer constraints (from RediSearch)

1. Documents are **`HASH` and `JSON`**. JSON values are owned by RedisJSON (a
   separate module), so the design must involve module-type cooperation.
2. **No RAM doubling.** RediSearch cannot keep a full duplicate of indexed
   fields — that would roughly double dataset memory. This rules out a
   "store everything in the index" approach.
3. This should be a **general** capability ("a snapshot across Redis"), useful
   beyond RediSearch, and ideally a stepping stone toward replacing some
   fork-based operations.
4. Result sets are **unbounded** for some queries (e.g. `FT.SEARCH idx *`,
   large `LIMIT`, `FT.AGGREGATE` cursors) and bounded (top-K) for others.

---

## 2. Why the existing mechanisms don't fit

Redis already snapshots the keyspace two ways; neither serves this use case.

- **`fork()` + copy-on-write** (`RM_Fork` → `redisFork(CHILD_TYPE_MODULE)`, and
  how `BGSAVE`/AOF-rewrite work). Gives a consistent point-in-time view with
  zero synchronization, but in a **separate process**: results can't live in the
  module's in-process memory, and you cannot fork per query at search QPS.
- **Thread-safe context + the GIL** (`RM_GetThreadSafeContext`,
  `RM_ThreadSafeContextLock`, `moduleGIL`). A module thread can touch the
  keyspace, but only under a single global mutex, serialized with the main
  thread — it sees the **live, mutable** dataset, not a snapshot.

A third naive option — **copy the needed keys under the GIL** — is fine for a
bounded set but re-freezes the main thread and doubles memory for large scans,
so it fails constraint #2 and #4.

### Why not do it purely at the module level?

A module *can* do an **eager bounded copy** today (copy a result page under the
GIL, read the copies on a thread) with no core changes. What it **cannot** do is
the **lazy, unbounded, no-doubling** case, because lazy copy-on-write requires
capturing a value's **pre-image the instant before it is mutated or freed**, and
the module API has no hook there:

- Keyspace notifications fire **after** the write — the old value is already gone.
- In-place aggregate mutation (`HSET` on an existing hash) produces **no free
  event** at all; nothing for a keymeta `free`/`unlink` hook to catch.
- `RM_RegisterCommandFilter` can intercept *commands* pre-dispatch, but it misses
  **expiry**, **eviction**, and **cross-module (JSON) values**, and it's a
  write-path tax implemented in the wrong layer.

The two things lazy COW needs — a **pre-write interception point** and making
**in-place mutation respect a "someone is reading this" signal** — both live in
the core write path by design, and there is deliberately no module hook there
(a per-write module callback would be a performance and correctness hazard).
So Tier 1 is best framed not as "a new feature" but as **exposing the one
interception point that was always missing**, cheaply and safely.

---

## 3. The trilemma and the decision

For keyspace-value reads you can pick **two** of these three:

| approach            | unbounded results | lock-free reads (no GIL) | no RAM doubling |
|---------------------|:-----------------:|:------------------------:|:---------------:|
| Pin-set             | ✗                 | ✓                        | ✓ (bounded only)|
| **Value-MVCC**      | ✓                 | ✗ (reads under GIL)      | ✓               |
| Pin-everything      | ✓                 | ✓                        | ✗ (doubles)     |
| Index-MVCC (Tier 3) | ✓                 | ✓                        | ✓               |

Because **no-doubling is a hard constraint** and **unbounded result sets are
real** (`FT.SEARCH idx *`), the only option below the full Tier 3 rewrite is
**value-MVCC with reads under the GIL**.

Crucially, reads-under-the-GIL is **RediSearch's existing model**: concurrent
search already reads the keyspace under the GIL in short bursts and yields
between them. The snapshot does not change that — it makes the reads
**consistent across the yields**, which is the actual defect today.

**Decision: implement value-MVCC as the primary mechanism.** (See the decision
log, §11.)

---

## 4. Design

### 4.1 Concepts

**Global keyspace version.** `server.keyspace_version` (uint64), monotonic,
bumped once per write command's effects (in the dirty/propagation path near
`signalModifiedKey` / `server.dirty`). A snapshot taken between commands cleanly
separates writes-before from writes-after at **command granularity**.

**Snapshot object** (created under the GIL, O(1) — nothing in the keyspace is
touched at creation):

```c
typedef struct keyspaceSnapshot {
    uint64_t version;    /* keyspace_version captured at creation */
    dict    *preserved;  /* sds keyname -> kvobj* : frozen pre-write value */
    int      gen;        /* generation id */
    struct RedisModule *owner;
    /* linkage in server.keyspace_snapshots */
} keyspaceSnapshot;
```

**Gate.** A counter `server.snapshots_open` guards the entire write-path hook:

```c
if (unlikely(server.snapshots_open)) snapshotPreserveIfNeeded(...);
```

so there is **zero cost when no snapshot is open**.

### 4.2 Preserve-on-write (copy-on-write into the version-store)

While `snapshots_open > 0`, at each write choke point, **before the value
changes**:

> For every open snapshot `S` with `S.version < keyspace_version` where
> `key ∉ S.preserved`, retain the current (pre-write) value into `S.preserved`.

- **In-place aggregate / module mutation** → dup the current value, install the
  copy as the **live** value (`dbReplaceValueWithLink`), and stash the
  **original** (now detached and immutable) into `S.preserved`. JSON values are
  duplicated via the module type's `copy2` callback (the same one `COPY` uses).
- **Overwrite / delete** → the old object is being replaced/freed anyway;
  `incrRefCount` it into `S.preserved` *before* the existing `decrRefCount`. No
  copy needed.
- **Strings** self-COW via `dbUnshareStringValue` once `refcount > 1`, so they
  only need the retain step.

**Sharing across snapshots.** Two snapshots with the same baseline for a key
reference the **same** frozen object by refcount — one copy, multiple refs. This
is automatic: "retain the current live object for any open snapshot that lacks
the key and predates this write." Snapshots at different baselines naturally hold
different objects.

### 4.3 Read resolution (`RM_SnapshotOpenKey`, under the GIL)

- If `key ∈ S.preserved` → return the frozen value.
- Else the key has **not changed since `V`**, so its live value *is* the
  as-of-`V` value → look it up live (safe because the read holds the GIL) and
  return it.

Delete-then-recreate is handled correctly: the delete already preserved the
pre-`V` value for `S`, so a later recreate under the same name does not disturb
what `S` sees.

### 4.4 Deferred free

Falls out of refcounting: a preserved object is held by `S.preserved`
(`refcount ≥ 1`). Live `DEL`/expiry/eviction only drop the keyspace reference;
the object survives for the snapshot and is freed (lazyfree-eligible) when the
snapshot is freed.

### 4.5 Thread model

Reads happen **under the GIL in bursts** — RediSearch's existing rhythm:

```
lock GIL
  for each doc in this batch:
      key = RM_SnapshotOpenKey(snap, docKey)
      read fields; copy what's needed into module memory
unlock GIL          // main thread runs, may write; preserve fires for `snap`
... score / aggregate / sort on the worker thread ...
lock GIL            // next batch, still consistent as-of V
```

Consequences:

- The version-store, the dict, and **all refcount mutations** are touched only by
  whoever holds the GIL → effectively single-threaded → **no atomics, no
  lock-free, no RCU**.
- Values returned by `RM_SnapshotOpenKey` are valid **only while the GIL is
  held**; the module must copy out anything it needs to keep across a yield.

### 4.6 Consistency semantics and the documented boundary

**This snapshots values, not the key-set.** A key *created* after `V` is visible
to the snapshot (existence is not versioned). This is acceptable because
**RediSearch's index is the authority** on which keys existed at `V` — it only
asks the snapshot for those keys, which did exist at `V`, and for each it gets
the correct as-of-`V` value. Snapshotting existence (so that keys born after `V`
are invisible, and full-keyspace iteration is possible) is **index-MVCC**, which
is deferred to Tier 3.

Guarantee to state to consumers: *all reads through a snapshot observe every
in-scope key's value as of the snapshot's `keyspace_version`, for keys that
existed at that version.*

---

## 5. Integration points (core)

All in `src/db.c` unless noted.

- **Version bump:** dirty/propagation path (near `signalModifiedKey` /
  `server.dirty`).
- **Preserve helper** `snapshotPreserveForWrite(db, key, kv)`, off **two central
  hooks**:
  1. **`lookupKeyWrite*` wrappers** (`lookupKeyWriteWithFlags` /
     `lookupKeyWriteWithLink`) — the single point every write-intent lookup passes
     through. On a snapshot-relevant hit we **freeze a detached side copy and
     leave the live value completely untouched**: deep-copy the value
     (`dupStringObject`/`listTypeDup`/`setTypeDup`/`zsetDup`/`hashTypeDup`/
     `streamDup`/`arrayTypeDup`, or the module type's `copy2`/`copy` for module
     values — a type registering neither is skipped), wrap it into a standalone
     `kvobj` via `kvobjSet` (re-embeds the key without installing it in the
     keyspace), and store that in the lacking snapshots. The command then mutates
     the original in place as usual.
     - **Why freeze a side copy instead of reinstalling the copy as live?**
       Reinstalling (an approach we rejected) would change the live object
       identity, so the `lookupKeyWrite*`
       wrappers had to refresh the caller's `dictEntryLink` (callers like
       `APPEND`/`SETRANGE` reuse it) and we had to reason about
       WATCH/replication/keyspace-notification interactions with the reinstall.
       Freezing a *detached* copy and leaving the live value alone removes all of
       that: the link and object identity stay valid, and the hook is provably
       invisible to everything except the snapshot. Same one-copy cost, strictly
       less surrounding work (no `dbReplaceValue`, no dict manipulation).
     - **Possible future — string *retain* optimization.** Strings *could* skip the
       copy: just `incrRefCount` the live value, and let `dbUnshareStringValue`
       clone lazily iff mutated in place. This is copy-free but keeps a *shared
       live* object, so it needs a `RENAME`/`MOVE`/realloc safety review first.
     - **Trade-off:** the frozen copy is built with `keyMetaBits = 0`, so it
       carries no TTL/metadata — snapshots are value-only. TTL-consistent
       snapshots would copy the expire into the frozen `kvobj` (future work).
  2. **`dbGenericDelete`** — *retain* the old value before it is freed. Covers
     `DEL`/`UNLINK`/expiry/eviction, i.e. removals that don't pass through a
     write-lookup. Idempotent via the version-store membership check, so a key
     already preserved by the `lookupKeyWrite` hook isn't preserved twice.
- **Gate:** `server.snapshots_open` checked before the helper (zero idle cost).
- **Version-store** stores the frozen `kvobj` directly, keyed by the key sds it
  already embeds (`kvobjGetKey`) — no duplicated key string. A single pass over
  the open-snapshot list gathers the snapshots that lack the key, so the deep copy
  happens at most once.
- **Scoping:** each snapshot may carry a key prefix/
  pattern; the hook early-outs when the key is out of scope for *every* open
  snapshot, so writes to keys the consumer will never read cost nothing. This is
  the highest-ROI optimization for the RediSearch workload (indexed keys are a
  subset of the keyspace).
- **defrag:** must **skip** `refcount > 1` objects (else it could relocate memory
  under a preserved pointer). Confirm/add guard in the defrag scan
  (`dictScanDefrag`, `src/dict.c`).
- **Module type COW:** reuse the `copy2` callback (`moduleTypeDupOrReply` path,
  `src/module.c`). If a module type registers neither `copy` nor `copy2`,
  preserving a value of that type must fail loudly.
- **Audits:**
  - Any in-place mutator that obtains its object via `lookupKeyRead` (not
    `lookupKeyWrite`) would bypass the hook — audit and route/forbid.
  - `RENAME`/`MOVE` relink the same pointer under a new name; the snapshot holds
    its own pointer, so this is expected-safe — verify.

### Reused precedents (nothing invented from scratch)

- Value-level COW already exists for strings: `dbUnshareStringValue`
  (`refcount != 1` ⇒ clone).
- Single write choke point: every mutation of an existing value flows through the
  `lookupKeyWrite*` family (or `RM_OpenKey` + `REDISMODULE_WRITE`). Genuine
  bypasses (RDB load, `RESTORE`, `COPY`, `RENAME`/`MOVE`) install **new** objects
  or relink a pointer — none mutate a shared value's bytes.
- Deferred free: the lazyfree/bio machinery (`freeObjAsync`, guarded by
  `refcount == 1`).
- Deep-copy of a module value: the `copy2` callback used by `COPY`.

---

## 6. Module API surface

`src/module.c`, registered in `redismodule.h`. **All must be called with the GIL
held** (main thread, or a thread holding the thread-safe context lock) —
lifecycle ops mutate the shared open-snapshot registry that the write-path hook
reads, and reads touch the keyspace.

```c
/* Create a value snapshot of the context's currently-selected DB (writes to
 * other DBs never preserve into it; reads resolve against that DB). `cfg` is a
 * versioned config struct — NULL for whole-DB defaults; cfg->prefix scopes it to
 * keys with that prefix. */
RedisModuleKeyspaceSnapshot *RM_CreateKeyspaceSnapshot(RedisModuleCtx *ctx,
                                    const RedisModuleKeyspaceSnapshotConfig *cfg);
/* Open a key as-of the snapshot (frozen copy if written/deleted since, else
 * live). READ-ONLY RedisModuleKey; close with RM_CloseKey. NULL if absent. */
RedisModuleKey *RM_SnapshotOpenKey(RedisModuleCtx *ctx,
                                   RedisModuleKeyspaceSnapshot *snap,
                                   RedisModuleString *keyname);
void RM_FreeKeyspaceSnapshot(RedisModuleCtx *ctx, RedisModuleKeyspaceSnapshot *snap);
```

All configuration goes through the **versioned `cfg` struct passed to the
constructor** (mirroring `RM_CreateDataType`'s methods struct), not per-option
setter functions. This keeps the API surface tiny as options accrue — follow-up
PRs add *fields* (type filter, size cap, hash-delta flag), not new functions —
and applies config **atomically at creation under the GIL, before any write can
be preserved**, so there's no "created-but-not-yet-configured" window in which a
write would be captured under the wrong config.

The opened key works with the existing read accessors (`RM_KeyType`,
`RM_HashGet`, `RM_ModuleTypeGetValue`, `RM_StringDMA` in read mode, value-length
/ iteration). It is valid only while the GIL is held **and** the snapshot is
alive; copy out anything to keep beyond that. Writes on a snapshot-opened key are
unsupported.

> **Read-accessor safety — frozen strings are stored RAW.** `RM_StringDMA`
> "unshares" (decodes) an EMBSTR/INT string even in read mode, which reinstalls
> the value against the *live* keyspace — fatal for a detached frozen object
> (`db.c:593 'link != NULL'` assert). So the preserve paths decode string values
> to `OBJ_ENCODING_RAW` before freezing (both the write-copy path via
> `snapshotDupValue`, and the delete-retain path, which copies strings instead of
> retaining them for this reason). Aggregate/module read accessors don't unshare,
> so those types are retained as-is on delete. Covered by
> `tests/unit/moduleapi/kvsnapshot.tcl`.

Scoping is via `cfg->prefix`. Richer scope controls (type filter, size cap) and
behavior flags (hash delta-log) arrive in follow-up PRs as additional `cfg`
fields, not new functions (see §7).

---

## 7. Delivery: PR stack and roadmap

The feature lands as a stack of small, independently-reviewable PRs against
`unstable`, each generic-first and justified on its own. Later PRs add
configuration **fields** (to the `cfg` struct) and opt-in behavior, never
new functions — so the API surface stays small as capability grows.

| PR | Content | Status |
|----|---------|--------|
| **1 — core (this PR)** | Generic value-MVCC: `keyspace_version`, the `snapshots_open` gate, the 3 write/delete hooks (§5), preserve-on-write deep-copy for all native types + module-type `copy2`/`copy`, DB scoping, `cfg->prefix` scoping, reads (main thread and background under the GIL), the RM API (`Create`/`GetVersion`/`OpenKey`/`Free`), and the `DEBUG KVSNAPSHOT` test surface. | proposed |
| **2 — scope controls** | Bound worst-case copy cost: `cfg` gains a **type filter** and a **size cap** (skip + count oversized values); `RM_SnapshotGetSkippedCount`. | ready |
| **3 — hash field delta-log** | Opt-in O(delta) hash writes (`cfg` flag): per-field inverse deltas with tombstones instead of deep-copying the whole hash; materialize for whole-doc read / delete; the size cap bounds the one O(size) step. | ready (spike hardened for HSET/HDEL/DEL) |
| **JSON — path-delta contract** | The proper JSON write-CPU fix: a module-type *preserve-strategy* callback set (`snapshot_reconstruct` / `snapshot_free_delta`) plus `RM_SnapshotRecordDelta`, so RedisJSON captures path-level inverse deltas and reconstructs as-of-snapshot — the delta-log's shape exposed to module types. Cross-team (a core-contract PR + a RedisJSON PR). | future |

Reads are always **under the GIL** — the lock-free variant from the earlier
pin-based sketch is gone.

> A core-only *serialized-freeze* for module values (freeze as an `rdb_save`
> blob, ~3.3× less frozen RAM than a tree copy) was prototyped as an interim
> memory win. It's **parked** in favor of the path-delta contract above, which
> also fixes write-CPU; the blob remains a viable representation for the
> collapse/delete copy that deltas still fall back to, if needed.

---

## 8. Risks and limits

- **Values-not-existence boundary** (§4.6) — correct for RediSearch, must be
  documented for general users.
- **Memory under a long-held snapshot** grows with write churn during the
  snapshot's life (same shape as fork COW). Mitigate with prefix/type scoping and
  the size cap (follow-up PR), or by aborting a snapshot past a memory threshold.
- **Over-copy on aggregate delete/overwrite:** the `lookupKeyWrite` hook copies
  aggregates eagerly, so a command that *looks up an aggregate for write but then
  deletes or wholesale-replaces it* pays a wasted deep copy (the frozen original
  is still correct). Strings avoid this entirely (retain, not copy); the common
  aggregate case (`LPUSH`/`HSET`/`SADD`/`ZADD` in-place mutation) is optimal. The
  `dbGenericDelete` retain hook keeps deletes cheap *only* when the delete path
  didn't already trigger a write-lookup; distinguishing "will delete/replace" at
  lookup time is a follow-up optimization.
- **Large JSON under write while a snapshot is open:** cloning a RedisJSON value
  is a **full recursive deep copy** — verified against redisjson v8.9.81: every
  array/object/number node is reallocated and children are cloned recursively
  (`IValue::clone`). Cost is O(container-node-count). It is *not* O(total bytes):
  string leaves are interned and atomically refcounted, so string payloads stay
  shared — but there is **no container-level COW/refcount** to exploit, so a
  first-write to a large/deep JSON doc under a snapshot pays a full structural
  clone. **Accepted for v1** (problematic but acceptable). Metrics will show
  if it becomes real pain.
- **23-bit refcount** (`OBJ_REFCOUNT_BITS`, `src/object.h`) is ample (~8M) but
  worth an assert on preserve.

---

## 9. Future work (Tier 3, out of scope here)

- **Index-MVCC**: version the key-set so keys born after `V` are invisible and a
  background thread can iterate the whole keyspace as-of `V`. This is what
  enables **lock-free unbounded reads** and **replacing fork** for
  RDB/AOF/replication (needs per-entry birth/death epochs, deferred compaction,
  and an epoch↔replication-offset binding).
- **Pin-set optimization**: for bounded queries wanting truly parallel (GIL-free)
  value reads, a refcount-pinned self-contained set. Add only if profiling shows
  GIL bursts hurt small queries.
- **Finer-grained module COW callback**: avoid full-document clones for large
  JSON under active write. Note this is **not just a core callback** — RedisJSON's
  value representation (`ijson::IValue`) has no container-level refcount today
  (arrays/objects are single-owner; only string leaves are refcounted), so true
  subtree sharing would require adding container refcounting/COW *inside*
  RedisJSON. That's a substantial RedisJSON-side change and a cross-team
  dependency, not a small core addition.
- **Structural sharing / cheaper frozen form**: `hashTypeDup` (and the
  other type dups) cost scales with value size and is worst for HT-encoded hashes
  (full dict rebuild → ~2× memory while a snapshot holds them). Options: field/
  node-level structural sharing (true COW), or freezing a compact *serialized*
  form for the read-only side (halves frozen memory at the cost of parsing on
  snapshot read). Both are out of scope for Tier 1/2; recorded so the cost profile
  is on the record.

---

## 10. Open questions

- Snapshot **sharing across concurrent queries** when no write intervened: the
  version-store + refcount design supports it; confirm RediSearch's preferred
  granularity (share one generation, or one per query).
- **Scoping** default: should a snapshot preserve for the whole keyspace or
  require an explicit key prefix/pattern? RediSearch indexes a subset, so scoping
  can dramatically cut COW cost.

---

## 11. Decision log

| Decision | Rationale |
|---|---|
| Solve in **core**, not the module | Lazy COW needs a pre-write interception point and refcount-aware in-place mutation; both live in the core write path, and there is deliberately no safe module hook there (§2). |
| **Value-MVCC** over pin-set / pin-everything / copy | Only option satisfying **unbounded results + no RAM doubling** below a full index-MVCC rewrite; reads-under-GIL match RediSearch's existing model (§3). |
| **Reads under the GIL** (not lock-free) | Removes atomics/RCU/memory-ordering hazards; the worker thread still runs query computation in parallel and only takes the GIL for value-fetch bursts, exactly as concurrent search does today (§4.5). |
| Snapshot **values, not key existence** | RediSearch's index is the key-set authority; existence-snapshotting is the harder index-MVCC problem and unnecessary for the consumer (§4.6). Deferred to Tier 3. |
| Reuse **`copy2`** for JSON, accept full-doc copy in v1 | No RedisJSON changes needed if `copy2` is registered; full-doc copy is "problematic but acceptable" for v1 per the consumer (§8). |
| Reuse **refcount + lazyfree** for deferred free | The machinery already exists and is single-threaded-safe under the GIL model (§4.4). |
| **`snapshots_open` gate** | Guarantees zero write-path overhead when no snapshot is open (§4.1). |
| **Two central hooks** (`lookupKeyWrite*` + `dbGenericDelete`), not per-mutator | Every write-intent lookup and every removal passes through these two points; scatter across each type's mutators is unnecessary and error-prone (§5). |
| **Freeze a detached side copy** (leave live untouched) | Freezing a detached copy and leaving the live value in place avoids reinstalling — no `dictEntryLink` refresh, no object-identity churn, and provably invisible to WATCH/replication/notifications. A copy-free *retain* keeps a *shared live* object exposed to `RENAME`/`MOVE`/realloc, needing a safety review first, so it's a possible future optimization (§5). |
| **Per-snapshot scoping in the core** | Highest-ROI optimization for the RediSearch workload — indexed keys are a subset, so scoping turns "copy on all writes" into "copy on indexed writes" (§5). |
| **Branch per milestone** off a base branch | Keeps each milestone independently reviewable/measurable (§7). |
