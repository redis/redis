# BLESS — code map (functions & structs)

A reference for the functions and types the BLESS feature uses. Every symbol
links to its source line.

## Table of contents

**[Structs & types](#structs--types)**
- [`redisDb.blessed_keys`](src/server.h#L1229) — per-DB index
- [`redisServer.bless_class_id`](src/server.h#L2519) — keymeta class id
- [`redisServer.bless_max_keys`](src/server.h#L2518) — cap config
- [`blessedDictType`](src/t_bless.c#L37) — dict type
- [`BLESS_NONE / NOEVICT / INRAM`](src/t_bless.c#L23) — level ladder
- [`KeyMetaClassConf`](src/keymeta.c#L729) — keymeta class config

**[Our functions (t_bless.c)](#our-functions-t_blessc)**
- [`blessInit()`](src/t_bless.c#L139) — register the class
- [`blessedDictCreate()`](src/t_bless.c#L48) — make a per-DB index
- [`blessedSetPut()`](src/t_bless.c#L54) — add/update in index
- [`blessedSetDel()`](src/t_bless.c#L63) — remove from index
- [`blessGetLevel()`](src/t_bless.c#L77) — read level
- [`blessTrackKey()`](src/t_bless.c#L72) — rebuild index on add
- [`blessNoEvict()`](src/t_bless.c#L83) — eviction guard
- [`blessNoSwap()`](src/t_bless.c#L89) — swap guard
- [`blessRdbSave()`](src/t_bless.c#L97) — persist level
- [`blessRdbLoad()`](src/t_bless.c#L102) — read level back
- [`blessUnlink()`](src/t_bless.c#L113) — remove on DEL/expire
- [`blessRename()`](src/t_bless.c#L121) — re-key on RENAME
- [`blessKeep()`](src/t_bless.c#L130) — keep on COPY/MOVE
- [`blessCommand()`](src/t_bless.c#L218) — container dispatcher
- [`blessSetCommand()`](src/t_bless.c#L163) — BLESS SET
- [`blessGetCommand()`](src/t_bless.c#L209) — BLESS GET
- [`blessLevelName()`](src/t_bless.c#L28) — level → text

**[Core / keymeta APIs we call](#core--keymeta-apis-we-call)**
- [`keyMetaClassCreate()`](src/keymeta.c#L729)
- [`keyMetaSetMetadata()`](src/keymeta.c#L794)
- [`keyMetaGetMetadata()`](src/keymeta.c#L863)

**[Core seams that call us (integration points)](#core-seams-that-call-us-integration-points)**
- [`dbAddInternal()`](src/db.c#L417) / [`dbAddRDBLoad()`](src/db.c#L531) — add hook → `blessTrackKey`
- [`keyMetaOnUnlink()`](src/keymeta.c#L292) → `blessUnlink`
- [`keyMetaOnRename()`](src/keymeta.c#L208) → `blessRename`
- [`evictionPoolPopulate()`](src/evict.c#L153) → `blessNoEvict`
- [`emptyDbStructure()`](src/db.c#L1041) — clear index
- [`dbSwapDatabases()`](src/db.c#L2664) — swap index
- [`initServer()`](src/server.c#L8129) — `blessInit` + per-DB dict
- [`config table`](src/config.c#L3373) — `bless-max-keys`

**[One-glance flow](#one-glance-flow)**

---

## Structs & types

| type | where | what it is / holds |
|------|-------|--------------------|
| [`redisDb.blessed_keys`](src/server.h#L1229) | server.h | **Per-DB index**: key name (sds) → level. Sits next to `db->expires`. The always-in-RAM structure all decisions read. |
| [`redisServer.bless_class_id`](src/server.h#L2519) | server.h | keymeta class id assigned to `"BLES"` at startup. |
| [`redisServer.bless_max_keys`](src/server.h#L2518) | server.h | The cap (config `bless-max-keys`, default 1024), per DB. |
| [`blessedDictType`](src/t_bless.c#L37) | t_bless.c | Dict type for `blessed_keys`: sds key (dup+free), level in the value pointer. |
| [`BLESS_NONE / NOEVICT / INRAM`](src/t_bless.c#L23) | t_bless.c | The level ladder (0/1/2); `NONE` is the reset sentinel (never persisted). |
| [`KeyMetaClassConf`](src/keymeta.c#L729) | keymeta.c/.h | Config passed to `keyMetaClassCreate`: `reset_value`, `flags`, lifecycle callbacks. Filled in `blessInit`. |
| `RedisModuleKeyOptCtx` | server.h | Context for the keymeta copy/rename/move/unlink callbacks: `from_key`, `to_key`, `from_dbid`, `to_dbid`. |
| `KeyMetaSpec` | keymeta.h | Collected metadata for a key being added; its `metabits` tells the add-hook the key has a BLES value. |

---

## Our functions (t_bless.c)

| function | called by | why | what it does |
|----------|-----------|-----|--------------|
| [`blessInit()`](src/t_bless.c#L139) | [`initServer()`](src/server.c#L8129) once at startup | register the class before any key loads | fills `KeyMetaClassConf`, calls `keyMetaClassCreate(NULL,"BLES",0,&conf)`, stores `bless_class_id` |
| [`blessedDictCreate()`](src/t_bless.c#L48) | [`initServer()`](src/server.c#L3103) per DB | give each `redisDb` its own index | `dictCreate(&blessedDictType)` |
| [`blessedSetPut()`](src/t_bless.c#L54) | `blessSetCommand`, `blessTrackKey` | record a key's level | `dictAdd`(sdsdup) / update |
| [`blessedSetDel()`](src/t_bless.c#L63) | `blessSetCommand`, `blessUnlink`, `blessRename` | drop a key from the index | `dictDelete` |
| [`blessGetLevel()`](src/t_bless.c#L77) | `blessGetCommand`, `blessNoEvict`, `blessNoSwap` | read a key's level, no flash access | `dictFind` → level or `NONE` |
| [`blessTrackKey()`](src/t_bless.c#L72) | [`dbAddInternal`](src/db.c#L453) / [`dbAddRDBLoad`](src/db.c#L571) hook | rebuild the index when a key arrives via load / RESTORE / migration / COPY / MOVE / RENAME | wraps `blessedSetPut` |
| [`blessNoEvict()`](src/t_bless.c#L83) | [`evictionPoolPopulate`](src/evict.c#L153) + [random branch](src/evict.c#L682) | eviction guard | `level ≥ NOEVICT` |
| [`blessNoSwap()`](src/t_bless.c#L89) | RoF swap-out selector (BigRedis fork) | swap guard (keep `INRAM` in RAM) | `level ≥ INRAM` |
| [`blessRdbSave()`](src/t_bless.c#L97) | keymeta framework on RDB/DUMP save | persist the level with the key | `rdbSaveLen(level)` |
| [`blessRdbLoad()`](src/t_bless.c#L102) | keymeta framework on load/RESTORE | read the level back | `rdbLoadLen` → `*meta`; return 1 |
| [`blessUnlink()`](src/t_bless.c#L113) | [`keyMetaOnUnlink`](src/keymeta.c#L292) on DEL/expire/overwrite | remove on the **main thread** (not `free`, which may run on a bg thread) | `blessedSetDel(server.db[ctx->from_dbid], from_key)` |
| [`blessRename()`](src/t_bless.c#L121) | [`keyMetaOnRename`](src/keymeta.c#L208) | drop old name; new added by add-hook | `blessedSetDel(old)`; return 1 |
| [`blessKeep()`](src/t_bless.c#L130) | keymeta copy/move callbacks | keep bless across COPY/MOVE | return 1 |
| [`blessCommand()`](src/t_bless.c#L218) | command table dispatch | entry for the `BLESS` container | dispatch on `argv[1]` → set/get/count/list/help |
| [`blessSetCommand()`](src/t_bless.c#L163) | `blessCommand` | `BLESS SET key LEVEL` | parse → `lookupKeyWrite` → cap → `keyMetaSetMetadata` + `blessedSetPut` |
| [`blessGetCommand()`](src/t_bless.c#L209) | `blessCommand` | `BLESS GET key` | `blessGetLevel` → reply name/nil |
| [`blessLevelName()`](src/t_bless.c#L28) | `blessGetCommand`, LIST | level → text | `switch` |

---

## Core / keymeta APIs we call

| function | where | why we call it |
|----------|-------|----------------|
| [`keyMetaClassCreate()`](src/keymeta.c#L729) | keymeta.c | register the `BLES` class (in `blessInit`); returns the class id |
| [`keyMetaSetMetadata()`](src/keymeta.c#L794) | keymeta.c | attach/update the per-key level (in `blessSetCommand`); may realloc + re-store the object |
| [`keyMetaGetMetadata()`](src/keymeta.c#L863) | keymeta.c | read the level just attached to a loaded key (in the add-hook) |
| `lookupKeyWrite()` | db.c | get the object for `BLESS SET` (swaps a cold key in under RoF) |
| `keyModified()` | db.c | WATCH / tracking invalidation + dirty, after a bless change |
| `rdbSaveLen` / `rdbLoadLen` | rdb.c | serialize/deserialize the level in the rdb callbacks |

---

## Core seams that call us (integration points)

| seam (core) | invokes | why |
|-------------|---------|-----|
| keymeta framework (via our `KeyMetaClassConf`) | `blessRdbSave/Load`, `blessUnlink`, `blessRename`, `blessKeep` | persistence + lifecycle of the per-key value |
| [`dbAddInternal()`](src/db.c#L453) / [`dbAddRDBLoad()`](src/db.c#L571) | [`blessTrackKey`](src/t_bless.c#L72) | rebuild the index when a blessed key becomes live (load/RESTORE/migration/COPY/MOVE/RENAME) |
| [`keyMetaOnUnlink()`](src/keymeta.c#L292) | [`blessUnlink`](src/t_bless.c#L113) | remove on DEL / expire / overwrite |
| [`keyMetaOnRename()`](src/keymeta.c#L208) | [`blessRename`](src/t_bless.c#L121) | re-key on RENAME |
| [`evictionPoolPopulate()`](src/evict.c#L153) + [random branch](src/evict.c#L682) | [`blessNoEvict`](src/t_bless.c#L83) | skip blessed keys as eviction victims |
| [`emptyDbStructure()`](src/db.c#L1041) | `dictEmpty(db->blessed_keys)` | wipe per-DB on FLUSH / DEBUG RELOAD / full sync |
| [`dbSwapDatabases()`](src/db.c#L2664) / `swapMainDbWithTempDb()` | swap the `blessed_keys` field | keep the index with its data across SWAPDB |
| [`initServer()`](src/server.c#L8129) | [`blessInit()`](src/t_bless.c#L139) + [`blessedDictCreate()`](src/t_bless.c#L48) | one-time setup: register class + per-DB indexes |
| [config table](src/config.c#L3373) | `createIntConfig("bless-max-keys", …)` | the per-DB cap |

---

## One-glance flow

```
BLESS SET ──► blessSetCommand ──► keyMetaSetMetadata (kvobj, source of truth)
                              └─► blessedSetPut       (db->blessed_keys, index)

RDB/RESTORE/migration ──► dbAdd* hook ──► keyMetaGetMetadata ──► blessTrackKey ──► index
DEL/expire/overwrite  ──► keyMetaOnUnlink ──► blessUnlink ──► blessedSetDel
eviction              ──► evictionPoolPopulate ──► blessNoEvict ──► blessGetLevel(index)
swap-out (RoF fork)   ──► selector ──► blessNoSwap ──► blessGetLevel(index)
```
