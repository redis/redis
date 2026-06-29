# Redis-Roaring Migration Contract

Issue [#34](https://github.com/aviggiano/redis/issues/34) is resolved for v1 by
the [final adjudication](https://github.com/aviggiano/redis/issues/34#issuecomment-4832039784):
use an external streaming migrator, for example `redis-bitmap-migrate`, as the
supported path from `aviggiano/redis-roaring` deployments to Redis native bitmap
values.

This repository provides that v1 tool as `tools/redis-bitmap-migrate.py`.

Proposal A is the v1 contract. Proposal B, an offline RDB transcoder, remains a
possible future expert mode. Bridge modules and core legacy RDB loaders are out
of scope for v1.

## Redis Core Boundary

The migrator runs outside Redis core. Redis core must not add:

- a legacy `reroaring` or `roaring64` module RDB loader;
- `R.*` or `R64.*` compatibility command prefixes;
- silent conversion of redis-roaring module payloads during normal RDB load.

The target Redis may accept normal native bitmap `DUMP` / `RESTORE` payloads.
The migration tool is responsible for reading legacy data, building target
native bitmap values, validating them, and committing them safely.

## Tool Usage

The tool defaults to a dry run and writes a durable JSON manifest:

```
tools/redis-bitmap-migrate.py \
  --source-host 127.0.0.1 --source-port 6379 \
  --target-host 127.0.0.1 --target-port 6380 \
  --manifest redis-bitmap-migrate-manifest.json
```

To perform writes, run it during a maintenance window or another verified final
pass and acknowledge that source writes are frozen:

```
tools/redis-bitmap-migrate.py \
  --source-host 127.0.0.1 --source-port 6379 \
  --target-host 127.0.0.1 --target-port 6380 \
  --manifest redis-bitmap-migrate-manifest.json \
  --apply --assume-frozen
```

Useful safety switches:

- `--replace` allows overwriting destination keys; the default is no-overwrite.
- `--cap-policy skip` records above-cap `roaring64` keys as skipped instead of
  failing the run.
- `--resume` resumes from an existing manifest.
- `--all-dbs` discovers DB indexes from `INFO keyspace`; otherwise `--db`
  defaults to DB 0 and may be repeated.
- `--key`, `--key-file`, and `--pattern` restrict the source key set.

## Correctness Model

The default correctness story is a maintenance-window migration:

1. Run the source Redis with redis-roaring loaded.
2. Freeze writes before the final pass.
3. Copy each selected key to the target through temporary keys.
4. Validate the temporary native bitmap value.
5. Atomically rename or replace the destination key.
6. Let the target Redis save a clean native RDB afterward.

An initial copy followed by a final write freeze is allowed. A dual-write or
change-tracking flow is allowed only when it ends with a verified final pass.
Best-effort live `SCAN` by itself is not a correctness guarantee.

## Export Requirements

The tool must understand both redis-roaring module data types:

- `reroaring`, the 32-bit `R.*` input type;
- `roaring64`, the 64-bit `R64.*` input type.

The implemented tool uses bounded and probed redis-roaring commands:
`R.RANGEINTARRAY` / `R64.RANGEINTARRAY`. It exports by ordinal ranges, bounds
each command result with `--page-size`, detects export failures or concurrent
cardinality changes, and records enough source state for validation and resume.

The v1 native bitmap contract is capped at max bit offset `4294967295`, which
corresponds to a logical length of 512 MiB. Above-cap `roaring64` inputs default
to failure. An explicit `skip` policy is allowed only when the manifest records
the skipped key and reports it loudly. There is no silent truncation and no v1
`split` mode.

## Import and Commit Requirements

The implemented fast path imports by writing native bitmap `RESTORE` payloads
into manifest-owned temporary keys. To avoid hand-rolling the native wire format,
it creates a tiny native build key on the target Redis, streams exported offsets
into that native key with `SETBIT`, obtains the target-generated `DUMP` payload,
and `RESTORE`s that payload into the manifest temp key before validation.

After import, the tool validates the temporary key before finalizing. Finalizing
uses an atomic rename or replace into the destination key. The tool must preserve
the source DB index and TTL, preferably as an absolute expire time in the
manifest so resume and audit do not depend on wall-clock guesses.

Overwrite behavior must be explicit. Temporary keys must be owned by the
manifest, safe to clean up on resume, and impossible to confuse with user data.

## Manifest Requirements

A durable manifest is mandatory for dry-run, resume, crash recovery, and audit.
Each key entry must include at least:

- DB index and key name;
- source type, `reroaring` or `roaring64`;
- TTL or absolute expire time;
- cardinality, min set offset, max set offset, and maximum observed offset;
- source digest or payload hash;
- destination key and temporary key;
- overwrite policy;
- validation result;
- commit state;
- error state, when present.

The manifest drives cleanup and resume. A resumed run must be able to distinguish
uncommitted temp keys, validated temp keys, committed destination keys, skipped
keys, and failed keys.

## Validation Requirements

Validation cannot rely only on `BITCOUNT`. The minimum validation set is:

- target type and encoding;
- cardinality;
- min and max set offsets;
- boundary `GETBIT` checks around the first and last set offsets;
- deterministic sampled `GETBIT` checks;
- `BITPOS` parity where it applies;
- full source-target offset diff for small keys.

Tool implementations may add stronger payload or digest validation, but the
checks above are the v1 floor.

## Cluster Requirements

Cluster support is explicit, not implicit. The v1 tool rejects cluster-enabled
sources or targets. If a future tool claims cluster support, it must run against
masters, use DB 0 only, require stable topology with no resharding during the
migration, route each key by slot, and create same-slot temporary keys so the
final rename remains atomic.

Tools that do not implement those guarantees should reject cluster targets
instead of performing a best-effort migration.

## Expected Test Matrix

Migration tooling should cover:

- `reroaring` 32-bit and `roaring64` sources;
- empty, sparse, dense, run-container-heavy, max-v1-bit, and above-cap values;
- TTL preservation, DB selection, and overwrite/no-overwrite behavior;
- temp-key cleanup and crash/resume idempotency at every manifest state;
- corrupt payloads, unsupported type names or encoding versions, and command
  export failures;
- validation parity using `BITCOUNT`, `BITPOS`, sampled `GETBIT`, min/max set
  offsets, and full offset diff for small keys;
- bounded paging through `R.RANGEINTARRAY` / `R64.RANGEINTARRAY` when those
  commands are used;
- cluster and hash-slot behavior only when the tool claims cluster support.

The Redis core test suite should continue proving the native bitmap value type,
`DUMP` / `RESTORE`, AOF, replication, and command semantics. Legacy
redis-roaring payload compatibility belongs in the migration tool tests, not in
Redis core.
