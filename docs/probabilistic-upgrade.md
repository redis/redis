# Upgrading RedisBloom keys to native Redis objects

Preserving existing keys is required for every type. Native loaders retain
the RedisBloom type identifiers and supported encoding versions; applications
do not need to delete, export/reinsert or recreate their keys.

## Safe cutover

1. Keep a recoverable backup and test the exact old server/module versions on
   a copy of production data.
2. Quiesce writes on the old server. Complete SAVE, or BGREWRITEAOF if AOF is
   the authoritative persistence source; check the persistence status for success.
3. Stop the old server cleanly. Keep its dataset, database selection and expiry
   metadata. Do not start two processes against the same persistence directory.
4. Start the native build with the same data directory and persistence settings.
   Remove the RedisBloom loadmodule directive and translate supported module
   settings to their core equivalents. Do not load both implementations.
5. Verify key counts/types, representative queries and absolute TTLs before
   resuming writes. Retain the backup for rollback.

An old Top-K incremental AOF records randomized commands, not their exact
outcome. A completed old-server rewrite or RDB snapshot is therefore required
for exact-state Top-K cutover. This is a legacy persistence limitation, not
permission to discard keys. Legacy binary-string truncation or incompatible
native-layout blobs likewise cannot be repaired by inventing missing data.

## Upgrade regression coverage

Each migrated family has a real old-module → core-only restart test covering
RDB, plain rewritten AOF and RDB-preamble AOF. Tests verify populated and empty
keys in two databases, contents, absolute TTLs, unrelated ordinary keys and
continued writes. Set BLOOM_ORACLE_SERVER and BLOOM_ORACLE_MODULE to the old
server and module, then run the relevant make test target.

Current local validation uses RedisBloom v8.11.81 on the same architecture.
Other historical releases and cross-architecture upgrades are not yet claimed
as validated. All five families, including t-digest, pass the same local tests.

`make test-probabilistic-upgrade` loads checked-in old-module dataset fixtures
in all three persistence modes without requiring an external module. Event CI
and the Ubuntu nightly run this check unconditionally. The fixture producer is
`tests/probabilistic/upgrade.py --generate`; source provenance is recorded in
the fixture JSON. Native t-digest writes encoding version 1 to preserve buffered
nodes and exact integer weights, while loading legacy version 0. An old module
cannot load new version-1 snapshots; use the pre-upgrade backup for rollback.
