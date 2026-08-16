# BLESS — protect keys from eviction / swapping

Mark a small set of keys as **blessed** so they survive memory pressure. Aimed at
customers who run one DB mostly as a cache but keep a few non-cache keys
(counters, a stream or two) that must never disappear when eviction kicks in.

## Levels

The protection is a single **ordered level** (a number, not a bitfield), so
stronger levels can be added later without changing storage. **Implemented today:
`NONE` and `NOEVICT`.** A stronger `INRAM` level is designed for but not yet
implemented (see "Future: adding INRAM/NOSWAP").

```
   NONE ───────────► NOEVICT ─ ─ ─ ─►  (INRAM)
  (0, unblessed)   (1, never evicted)   (2, planned: never evicted
                                          AND never swapped to flash)
```

| level | evictable (`maxmemory`)? | swappable to flash (RoF)? | status |
|-------|--------------------------|---------------------------|--------|
| `NONE`    | yes | yes | ✅ |
| `NOEVICT` | **no** | yes | ✅ |
| `INRAM`   | **no** | **no** (pinned in RAM) | 🔜 planned |

## Commands

`BLESS` is a container command (like `OBJECT`):

```
BLESS SET <key> NONE|NOEVICT         # set the level; NONE clears (unbless)
BLESS GET <key>                      # -> "NOEVICT", or nil if not blessed
BLESS COUNT                          # number of blessed keys in the CURRENT db
BLESS LIST                           # map: key -> level, for the CURRENT db
BLESS HELP
```

Config: `bless-max-keys` (per DB, default 1024).

## Future: adding INRAM/NOSWAP (minimal effort)

Because the level is stored as a **number** (in the keymeta value + the RAM
index), adding the stronger level is purely additive — no storage, index,
persistence, or migration change:

1. `#define BLESS_INRAM 2`
2. accept `INRAM` in `blessSetCommand` + add the token to `bless-set.json`
3. add `blessNoSwap(db,key) { return blessGetLevel(db,key) >= BLESS_INRAM; }`
4. have the BigRedis swap-out selector call `blessNoSwap()` (see "BigRedis / RoF integration")

The rest of this document describes the full design **including** that future
`INRAM`/`NOSWAP` level, so the current `NOEVICT`-only build is a strict subset.

---

## The one idea

Bless is a **durable per-key property, modeled exactly like TTL.** TTL is keymeta
class 0; bless is keymeta class `BLES`. That single decision buys persistence and
migration transport for free. And like TTL, it lives in two places:

```mermaid
flowchart LR
  subgraph K["per key"]
    KM["keymeta BLES value<br/>(in the kvobj)<br/><b>source of truth</b><br/>durable · migrates"]
  end
  subgraph D["per redisDb"]
    IX["blessed_keys dict<br/>name → level<br/><b>derived index</b><br/>always in RAM"]
  end
  KM -- "rebuilt on load / migration" --> IX
  IX -- "O(1) lookup for decisions" --> Q(["eviction / swap / COUNT / LIST"])
```

- **keymeta value** — the source of truth. Durable, migrates, may page to flash
  with its value.
- **`db->blessed_keys`** — a derived, always-resident index. Every decision and
  `COUNT`/`LIST` read *this*, so a cold (on-flash) key is still decided on without
  a flash read.

This mirrors TTL exactly: keymeta expire (class 0) is the source of truth, and
`db->expires` is the derived per-DB index.

---

## Layered view

```mermaid
flowchart TB
  subgraph L1["Layer 1 · Command surface — t_bless.c, commands/*.json, config.c"]
    A["BLESS SET/GET/COUNT/LIST · bless-max-keys"]
  end
  subgraph L2["Layer 2 · Durable storage (source of truth)"]
    B["keymeta class BLES — level in the kvobj<br/>RDB · DUMP/RESTORE · slot migration"]
  end
  subgraph L3["Layer 3 · RAM index (derived, per-DB, always resident)"]
    C["db->blessed_keys : name → level"]
  end
  subgraph L4["Layer 4 · Enforcement (guarding)"]
    D["NOEVICT → performEvictions (core)<br/>NOSWAP → RoF swap-out selector (BigRedis)"]
  end
  A -- "keyMetaSetMetadata()" --> B
  A -- "blessedSetPut/Del()" --> C
  B -- "dbAdd* hook → blessTrackKey()" --> C
  C -- "blessNoEvict(db,key) / blessNoSwap(db,key)" --> D
```

### Function interaction map

The actual functions and how they cross the layers:

```mermaid
flowchart TD
  cmd["blessCommand()"] --> setc["blessSetCommand()"]
  setc -- "attach level (L2)" --> kms["keyMetaSetMetadata(db,o,BLES,level)"]
  setc -- "update index (L3)" --> put["blessedSetPut(db,key,level)"]

  save["keymeta rdb_save"] --> brs["blessRdbSave() → rdbSaveLen(level)"]
  load["keymeta rdb_load"] --> brl["blessRdbLoad() → rdbLoadLen(level)"]

  add["dbAddInternal / dbAddRDBLoad"] -- "reads attached level (L2→L3)" --> track["blessTrackKey(db,key,level)"]
  track --> put
  unlink["keyMetaOnUnlink → blessUnlink()"] --> del["blessedSetDel(db,key)"]
  rename["keyMetaOnRename → blessRename()"] --> del

  evict["performEvictions → evictionPoolPopulate"] --> ne["blessNoEvict(db,key)"]
  ne --> get["blessGetLevel(db,key) ≥ NOEVICT"]
  put --> IX[("db->blessed_keys")]
  del --> IX
  get --> IX
```

- **L1 command** writes *both* the keymeta value (L2) and the index (L3).
- **L2 load/migration** funnels through the `dbAdd*` hook, which reads the freshly
  attached level and calls `blessTrackKey` to rebuild L3.
- **L3 removals** ride the main-thread `unlink`/`rename` keymeta callbacks.
- **L4 guards** are pure O(1) reads of L3.

---

## Write path — `BLESS SET k INRAM`

```mermaid
sequenceDiagram
  autonumber
  participant C as client
  participant Cmd as blessCommand / blessSetCommand
  participant KV as kvobj (keymeta, L2)
  participant IX as db->blessed_keys (L3)
  C->>Cmd: BLESS SET k INRAM
  Cmd->>Cmd: parse level (NONE/NOEVICT/INRAM)
  Cmd->>KV: lookupKeyWrite(k)  (swaps in if on flash)
  Cmd->>Cmd: cap check: dictSize(db->blessed_keys) < bless-max-keys
  Cmd->>KV: keyMetaSetMetadata(db, o, BLES, INRAM)
  Cmd->>IX: blessedSetPut(db, k, INRAM)
  Cmd->>C: +OK  (keyModified, notify, dirty++)
```

`NONE` takes the mirror path: `keyMetaSetMetadata(..., NONE)` (reset sentinel →
never persisted) + `blessedSetDel(db, k)`.

---

## Load / migration path — rebuilding the index from the keys

The keymeta `rdb_load` callback has **no key name** (the key isn't live yet), so
the index is rebuilt at the *key-add* choke points, not in the callback.

```mermaid
sequenceDiagram
  autonumber
  participant Src as RDB file / source shard
  participant RL as rdbResolveKeyType → blessRdbLoad
  participant ADD as dbAddInternal / dbAddRDBLoad
  participant IX as db->blessed_keys (L3)
  Src->>RL: RDB_OPCODE_KEY_META + level  (before the key)
  RL->>RL: read level into KeyMetaSpec
  RL->>ADD: attach level to the new kvobj
  ADD->>ADD: keyMetaGetMetadata(BLES) → level
  ADD->>IX: blessTrackKey(db, key, level)
```

RESTORE / COPY / MOVE / RENAME-new go through `dbAddInternal`; RDB file load goes
through `dbAddRDBLoad`; both carry the hook, so cold (on-flash) keys are rebuilt
too — their level rides in the RAM-resident metadata, not with the flash value.

---

## Keeping the index in sync (main thread only)

```mermaid
flowchart LR
  BLESS["BLESS SET"] -->|add / update / remove| SET[("db->blessed_keys")]
  ADD["dbAddInternal / dbAddRDBLoad<br/>RDB · RESTORE · migration · COPY · MOVE · RENAME-new"] -->|blessTrackKey| SET
  UNLINK["unlink cb — DEL · expire · overwrite"] -->|blessedSetDel| SET
  RENAME["rename cb"] -->|remove old name| SET
  FLUSH["emptyDbStructure — FLUSHDB/ALL · DEBUG RELOAD · full sync"] -->|dictEmpty| SET
  SWAP["SWAPDB"] -->|swap field with the data| SET
```

- **Remove** uses the `unlink` callback (main thread), never `free` (which may run
  on a lazyfree background thread and must not touch the DB).
- **Flush/reload** clears per-DB inside `emptyDbStructure`, so an empty-then-load
  never leaves stale entries.
- **SWAPDB** swaps `db->blessed_keys` alongside `keys`/`expires` — the index
  follows the data.

---

## What lives in RAM vs. flash (under RoF)

| state | where | on flash? | used for |
|-------|-------|-----------|----------|
| BLES level | inline in the `kvobj` value | **yes** — pages out with the value | durability, migration |
| `metabits` presence bit | in the `kvobj` header | rides with the value | marks the slot present |
| `db->blessed_keys` index | per-DB `dict` | **no** — always resident | every decision + COUNT/LIST/cap |

```mermaid
flowchart LR
  subgraph RAM
    IX["db->blessed_keys<br/>counter → NOEVICT<br/>job → INRAM<br/>(decisions read here)"]
    HDR["cold key's RAM record<br/>(level available via meta CF)"]
  end
  subgraph FLASH["FLASH (cold values)"]
    VAL["value blob<br/>[ BLES level ][ robj ][ … ]"]
  end
  IX -. "mirror" .- HDR
  HDR -. "value paged out" .- VAL
```

The level is *stored* wherever the value is; it is *decided on* only from the
always-resident index. That's why a cold blessed key is protected without a flash
read.

---

## Eviction & swap decision

Two independent pressure sources; the level is read once from the index:

```mermaid
flowchart TD
  P[Memory pressure] --> S["sample candidate key<br/>(RAM-resident records)"]
  S --> L{"level = blessGetLevel(db,key)<br/>O(1), RAM only"}
  L --> PT{pressure type}
  PT -->|maxmemory: want to EVICT| EV{"level ≥ NOEVICT ?"}
  PT -->|RoF RAM pressure: want to SWAP| SW{"level ≥ INRAM ?"}
  EV -->|yes| SKIP1["skip → pick another victim"]
  EV -->|no| EVICT["evict (delete)"]
  SW -->|yes| SKIP2["keep in RAM → pick another"]
  SW -->|no| SWAP["swap value to flash"]
```

**Safety:** if a sampled batch is entirely `NOEVICT`+, `evictionPoolPopulate`
reports zero candidates, so eviction stops and the client gets a normal `OOM`
error instead of the server spinning. Verified under load.

---

## Persistence & migration (free, via keymeta)

```mermaid
sequenceDiagram
  autonumber
  participant Src as Source shard
  participant Dst as Dest shard
  Src->>Src: DUMP — rdbSaveKeyMetadata writes RDB_OPCODE_KEY_META + level
  Src->>Dst: RESTORE key <payload>
  Dst->>Dst: rdbResolveKeyType → blessRdbLoad → level
  Dst->>Dst: dbAddInternal attaches level + blessTrackKey → index
  Note over Src,Dst: source trim (dbDelete) → unlink cb removes it from source index
```

Same machinery for RDB save/load and cluster slot migration; no module-aux, no
separate channel. `NONE` keys carry nothing (reset sentinel is never serialized).

---

## BigRedis / RoF integration (`NOSWAP`)

The swap engine (BigRedis, in the `redislabsdev/Redis` fork) owns RAM↔flash. Its
swap-out victim selector already skips keys in its own per-DB set `io_blessed_keys`
("keys that must stay in RAM"). It does **not** read core keymeta.

**Recommended wiring — the engine queries core, no mirroring:**

```mermaid
flowchart TD
  V["swap-out selector picks candidate key"] --> A{"in io_blessed_keys?<br/>(engine's own size-pins)"}
  A -->|yes| KEEP1["keep in RAM"]
  A -->|no| B{"blessNoSwap(db,key)?<br/>(core query — user INRAM)"}
  B -->|yes| KEEP2["keep in RAM"]
  B -->|no| SWAPOUT["swap value to flash"]
```

- **User `INRAM`** → the engine calls core `blessNoSwap(db,key)` at decision time.
  Nothing is copied into `io_blessed_keys`; single source of truth (core keymeta).
- **`io_blessed_keys`** keeps only the engine's *own* size-driven pins (values too
  big for flash / >4 GB). Core knows nothing about those; they stay engine-local.
- On load there's nothing to rebuild for user pins (core index rebuilds from
  keymeta); the engine re-derives size-pins from value sizes.

Caveat: the query must be safe from the thread that runs victim selection
(main-thread → fine).

*(Alternative, not preferred: mirror user `INRAM` into `io_blessed_keys` as a
"hard-bless" (size 0, never auto-unblessed). Reuses the existing skip but
duplicates state and needs load-time reconciliation.)*

### `NOEVICT` in the fork

`NOEVICT` can't piggyback on `io_blessed_keys`: the fork's `maxmemory`
delete-eviction path (`performEvictionsEx`, `ram_eviction == 0`) doesn't consult
it, and the flash-eviction path (`performFlashEvictionsEx`, `bigredis-evex.c`)
deletes cold keys entirely. Both must skip `NOEVICT` keys — the core guard we
added in `evict.c` plus a matching skip in the engine's flash-eviction candidate
scan. See "core vs BigRedis ownership" below.

### Core vs BigRedis ownership

```mermaid
flowchart LR
  subgraph core["CORE team (this repo)"]
    c1["BLESS command + keymeta BLES"]
    c2["db->blessed_keys index"]
    c3["NOEVICT in performEvictions / evictionPoolPopulate"]
    c4["blessNoEvict() / blessNoSwap() helpers"]
  end
  subgraph eng["BigRedis team (fork)"]
    e1["swap-out selector → call blessNoSwap()"]
    e2["flash-eviction scan → skip NOEVICT"]
    e3["io_blessed_keys = size-pins only"]
  end
  c4 --> e1
  c4 --> e2
```

---

## Group blessing by prefix (proposed — NOT implemented)

Bless many keys by **key-name prefix**, e.g. every key under `user:`. Design only.

```
BLESS GROUP <prefix> NONE|NOEVICT|INRAM   # rule: bless keys starting with <prefix>
BLESS GROUPS                              # list rules -> level
BLESS GROUPSCAN <cursor>                  # (Approach A) iterate live matches, non-blocking
```

Two candidate approaches, differing in **where the O(N) keyspace cost lands**:

```mermaid
flowchart LR
  subgraph A["Approach A — rule-only (RECOMMENDED)"]
    a1["store rule in a rax:<br/>prefix → level"] --> a2["decide at lookup:<br/>blessNoEvict = index OR rax prefix match"]
    a2 --> a3["BLESS GROUP = O(1)<br/>future keys covered free"]
  end
  subgraph B["Approach B — materialize"]
    b1["scan keyspace,<br/>stamp each match"] --> b2["COUNT/LIST cheap<br/>(real entries)"]
    b2 --> b3["O(N) on every GROUP/AUFERRE<br/>+ origin tracking"]
  end
```

| | A · rule-only | B · materialize |
|---|---|---|
| `BLESS GROUP` cost | **O(1)** | O(N) keyspace scan |
| future keys | automatic | per-add rule check + stamp |
| enumerate members | cursor only (`GROUPSCAN`) | **O(result), cheap** |
| memory | just the rules | + 8 bytes per matched key |
| removal | O(1) | O(N) re-scan + origin bookkeeping |

**Recommendation: Approach A** (rule-only, `rax` of `prefix → level`, evaluated at
the decision points). Same structure client-side tracking uses for `BCAST`
prefixes. Effective protection on a key = `max(per-key level, matching rule
levels)`.

**Common concerns:** rules are node config → persist like `FUNCTION` (own RDB
section + command propagation), replicate to every shard; cap the *number of
rules* (a prefix matches unbounded keys); a group `INRAM` maps to the same RoF
path as a per-key `INRAM`.

---

## Code reference (structs & functions)

### Types & storage

```c
/* t_bless.c — the level ladder (stored as the keymeta value) */
#define BLESS_NONE     0   /* unblessed; reset sentinel, never persisted */
#define BLESS_NOEVICT  1   /* never evicted, may swap to flash */
#define BLESS_INRAM    2   /* never evicted, always in RAM (implies NOEVICT) */

/* server.h — per-DB derived index, sits right next to db->expires */
typedef struct redisDb {
    kvstore *keys;
    kvstore *expires;
    dict    *blessed_keys;   /* key name (sds) -> level (in the value ptr) */
    /* ... */
} redisDb;

/* server.h — process-wide state */
struct redisServer {
    int bless_max_keys;      /* cap per DB (config, default 1024) */
    int bless_class_id;      /* keymeta class id assigned to "BLES" */
    /* ... */
};

/* t_bless.c — the per-DB dict type (sds key, level packed in the value ptr) */
static dictType blessedDictType = {
    dictSdsHash, NULL, NULL, dictSdsKeyCompare, dictSdsDestructor, NULL, NULL
};
dict *blessedDictCreate(void) { return dictCreate(&blessedDictType); }
```

### Index helpers (Layer 3)

```c
/* t_bless.c */
static void blessedSetPut(redisDb *db, sds name, uint64_t level);   /* add/update */
static void blessedSetDel(redisDb *db, sds name);                   /* remove */
static uint64_t blessGetLevel(redisDb *db, sds name);               /* -> level or NONE */

void blessTrackKey(redisDb *db, sds name, uint64_t level) {          /* public: dbAdd* hook */
    blessedSetPut(db, name, level);
}
int blessNoEvict(redisDb *db, sds name) { return blessGetLevel(db,name) >= BLESS_NOEVICT; }
int blessNoSwap (redisDb *db, sds name) { return blessGetLevel(db,name) >= BLESS_INRAM;  }
```

### keymeta class BLES — source of truth (Layer 2)

```c
/* t_bless.c — persistence rides in the keymeta value; NONE is never written */
static void blessRdbSave(RedisModuleIO *io, void *r, uint64_t *meta) {
    UNUSED(r);
    if (rdbSaveLen(io->rio, *meta) == -1) io->error = 1;
}
static int blessRdbLoad(RedisModuleIO *io, uint64_t *meta, int encver) {
    UNUSED(encver);
    uint64_t v = rdbLoadLen(io->rio, NULL);
    if (v == RDB_LENERR) { io->error = 1; return -1; }
    *meta = v; return 1;                 /* attach; index rebuilt later in dbAdd* */
}
/* main-thread removal; NOT free() (which may run on a lazyfree bg thread) */
static void blessUnlink(struct RedisModuleKeyOptCtx *ctx, uint64_t *meta) {
    if (ctx->from_key && ctx->from_dbid >= 0)
        blessedSetDel(&server.db[ctx->from_dbid], ctx->from_key->ptr);
}
static int blessRename(struct RedisModuleKeyOptCtx *ctx, uint64_t *meta); /* drop old name */
static int blessKeep  (struct RedisModuleKeyOptCtx *ctx, uint64_t *meta); /* copy/move: keep */

void blessInit(void) {                    /* called once, after keyMetaInit(), before RDB load */
    KeyMetaClassConf conf; memset(&conf, 0, sizeof(conf));
    conf.flags       = (1u << KEY_META_FLAG_ALLOW_IGNORE);  /* old peers skip gracefully */
    conf.reset_value = BLESS_NONE;
    conf.rdb_save = blessRdbSave; conf.rdb_load = blessRdbLoad;
    conf.unlink   = blessUnlink;  conf.rename   = blessRename;
    conf.copy     = blessKeep;    conf.move     = blessKeep;
    server.bless_class_id = keyMetaClassCreate(NULL, "BLES", 0, &conf);
}
```

### Commands (Layer 1)

```c
/* t_bless.c — container dispatcher (OBJECT-style); arity/keys per subcommand JSON */
void blessCommand(client *c) {
    const char *sub = c->argv[1]->ptr;
    if      (!strcasecmp(sub,"set"))   blessSetCommand(c);
    else if (!strcasecmp(sub,"get"))   /* blessGetLevel(c->db, argv[2]) -> name or nil */;
    else if (!strcasecmp(sub,"count")) addReplyLongLong(c, dictSize(c->db->blessed_keys));
    else if (!strcasecmp(sub,"list"))  /* map over c->db->blessed_keys: name -> level */;
    else if (!strcasecmp(sub,"help"))  addReplyHelp(c, ...);
    else addReplySubcommandSyntaxError(c);
}

static void blessSetCommand(client *c) {          /* BLESS SET key NONE|NOEVICT|INRAM */
    uint64_t level = /* parse c->argv[3] */;
    robj *o = lookupKeyWrite(c->db, c->argv[2]);  /* swaps a cold key in */
    if (!o) { addReplyErrorObject(c, shared.nokeyerr); return; }
    sds key = c->argv[2]->ptr;
    int already = dictFind(c->db->blessed_keys, key) != NULL;
    if (level == BLESS_NONE) {
        keyMetaSetMetadata(c->db, o, server.bless_class_id, BLESS_NONE);  /* sentinel */
        blessedSetDel(c->db, key);
    } else {
        if (!already && (int)dictSize(c->db->blessed_keys) >= server.bless_max_keys)
            { addReplyError(c, "reached the maximum number of blessed keys"); return; }
        keyMetaSetMetadata(c->db, o, server.bless_class_id, level);       /* source of truth */
        blessedSetPut(c->db, key, level);                                /* derived index */
    }
    keyModified(c, c->db, c->argv[2], NULL, 1); server.dirty++; addReply(c, shared.ok);
}
```

### Integration points (where core is touched)

```c
/* server.c  initServer() — create the per-DB index next to keys/expires */
server.db[j].blessed_keys = blessedDictCreate();

/* db.c  dbAddInternal() and dbAddRDBLoad() — rebuild the index on load/RESTORE/migration */
if (server.bless_class_id > 0 && (metabits & (1u << server.bless_class_id))) {
    uint64_t level;
    if (keyMetaGetMetadata(server.bless_class_id, kv, &level) && level != 0)
        blessTrackKey(db, key /* ->ptr */, level);
}

/* db.c  emptyDbStructure() — wipe per-DB on flush / DEBUG RELOAD / full sync */
dictEmpty(dbarray[j].blessed_keys, NULL);

/* db.c  dbSwapDatabases() / swapMainDbWithTempDb() — index follows the data */
db1->blessed_keys = db2->blessed_keys;
db2->blessed_keys = aux.blessed_keys;

/* evict.c  evictionPoolPopulate() and the random-policy branch — NOEVICT guard */
if (blessNoEvict(db, key)) continue;   /* skip; not counted -> clean OOM if all blessed */

/* config.c  the cap */
createIntConfig("bless-max-keys", NULL, MODIFIABLE_CONFIG, 0, INT_MAX,
                server.bless_max_keys, 1024, INTEGER_CONFIG, NULL, NULL);
```

Files: `t_bless.c` (feature), `commands/bless*.json` (command table), `server.h`
(struct fields + prototypes), `server.c` (per-DB creation + `blessInit()`),
`db.c` (add hooks, empty, swapdb), `evict.c` (NOEVICT guard), `config.c` (cap).

---

## Scope

**Implemented (this repo):**

- Layers 1–3: `BLESS SET/GET/COUNT/LIST`, the `bless-max-keys` cap, keymeta `BLES`
  storage (enum level), the per-DB `db->blessed_keys` index, and RDB +
  DUMP/RESTORE + slot-migration transport.
- Layer 4 `NOEVICT`: enforced in `performEvictions` for both the pool
  (LRU/LFU/volatile-ttl) and random policies, with correct OOM termination.
  `blessNoEvict(db, key)`.
- Per-DB + SWAPDB: `db->blessed_keys` (like `db->expires`); `COUNT`/`LIST`/cap per
  current DB; `SWAPDB` swaps the index with the data; cleared per-DB in
  `emptyDbStructure` (no stale entries after reload).

**Deferred:**

- Layer 4 `NOSWAP`: enforced in the BigRedis fork (see RoF integration). Core-side
  `blessNoSwap(db,key)` is ready.
- `NOEVICT` in the fork's flash-eviction scan (engine-side skip).
- `aof_rewrite` keymeta callback: bless survives RDB / replication / migration,
  but not an AOF rewrite yet.
- Group blessing by prefix — designed above, not implemented.

**Open questions to decide:**

- **Does bless survive a value overwrite?** `SET`/`GETSET`/`RESTORE REPLACE`/
  `COPY REPLACE`/`RENAME`-onto-existing build a fresh object without the old
  keymeta, so today bless is dropped (same as `SET` clearing a TTL). Preserve it
  (like `KEEPTTL`) or keep the TTL-like clear-on-overwrite? Undecided.
- **OOM vs. `INRAM` — who wins?** Under RoF, if the smallest value the relief
  valve wants to swap out is a user `INRAM` key: honor `INRAM` (risk node OOM),
  let OOM win (break the pin as last resort), or hybrid (honor to a limit, then
  swap + warn)? Undecided.
- **Cap on import:** migration/RDB load add to the index without a cap check, so a
  shard can exceed `bless-max-keys`. Treat the cap as a soft, command-time guard
  (migration/load may exceed) — proposed, matches current behavior.
