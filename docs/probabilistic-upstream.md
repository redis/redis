# RedisBloom upstream provenance

The native probabilistic implementations track RedisBloom `master`, audited at
commit `2fa3a19cbf0a63d9b7f8eba1e92d5fe5b24b2e0c` on 2026-10-10.
Upstream: https://github.com/RedisBloom/RedisBloom/tree/2fa3a19cbf0a63d9b7f8eba1e92d5fe5b24b2e0c

The initial migration used v8.11.81 (`d13bffe9c94955961e967911608acadedf40144b`).
The master comparison includes source, vendored algorithms, command metadata,
and unit/flow tests. New relevant changes are incorporated:

- MOD-18487: bound restored Bloom hash counts, with both algorithm boundary
  tests and the crafted BF.LOADCHUNK regression.
- MOD-18559: CF.COMPACT documentation and complexity. Native key specifications
  already identify its writable key.
- MOD-18948: per-write-command replication matrix, adapted to native ACL
  discovery. Also cover CF.COMPACT and require exact Top-K DUMP equality.
- MOD-19291: native tests use the existing bounded startup/shutdown harness;
  RLTest dependency and runner changes are not needed.

CMS, Cuckoo, Top-K, and t-digest algorithm sources have no additional changes
between that initial source revision and this master revision. Native allocator,
command, persistence, and correctness adaptations remain intentional differences;
this is not a byte-for-byte copy of the module.

Module packaging, Docker, and module-only CI changes do not apply to the native
build. They do not introduce new dependencies here.

The old-module upgrade oracle and checked-in fixtures remain v8.11.81 on purpose:
they verify that older persisted keys load in the new implementation. Source
provenance and upgrade-test provenance are separate. See
[upgrade validation](probabilistic-upgrade.md) for tested formats and limits.
