# Native Cuckoo filters

This PR targets `feature/native-probabilistic` independently; see
[the development workflow](probabilistic-development.md).

This independent change adds all 14 `CF.*` commands as a native Redis object.
Command handlers and object lifecycle live in `src/probabilistic/t_cuckoo.c`; the reusable
algorithm is in `src/probabilistic/cuckoo.c` and dump/chunk helpers are in `src/probabilistic/cf.c`.
No additional dependencies or module loading are required.

`BUILD_BLOOM=yes` enables the migrated families present in this checkout.
Do not load external RedisBloom into this build: command names conflict.

## Persistence and lifecycle

The existing `MBbloomCF` RDB encoding is retained (versions 0–4 load).
`CF.SCANDUMP` / `CF.LOADCHUNK` are compatible with RedisBloom. Native objects
support COPY, expiry, UNLINK, memory tracking, digest and active defragmentation.
AOF rewriting uses RESTORE, including empty filters. Error-path autocreation
propagates the empty reservation rather than replaying a rejected insertion.

`TYPE` returns `cuckoo`; the ACL category is `@cuckoo`. Keyspace notifications
use `C` (included in `A`). The five `cf-*` configuration settings retain module
defaults; see `redis.conf`.

## Tests

Run `make test-cuckoo` (also included by `make test`). This runs nine algorithm
tests and the migrated upstream flow tests plus native lifecycle, AOF, ACL,
notifications, RESP3, replication and optional external compatibility cases.

Set `BLOOM_LARGE_TESTS=1` for large allocation and overflow stress cases.
Defragmentation requires jemalloc. External compatibility requires
`BLOOM_ORACLE_SERVER` and `BLOOM_ORACLE_MODULE` pointing to an external-module
server and RedisBloom library. The upstream disabled insufficient-memory case
remains skipped. CI runs ordinary suites under jemalloc and ASan; the Ubuntu
nightly additionally enables large cases.
