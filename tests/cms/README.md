# Native CMS tests

Run `make test-cms` (or `make -C src test-cms`). The default `make test`,
Ubuntu CI, AddressSanitizer CI, and the Daily Ubuntu job also run this suite.
Use the same compiler/allocator flags as the server build.

## Migration inventory

- RedisBloom v8.11.81 `tests/flow/test_cms.py`: all 36 cases retained.
  The cluster-only case is explicitly skipped by the standalone harness;
  native `COMMAND GETKEYS` checks verify MERGE destination/source metadata.
- `rdb_corruption_utils.py`: copied upstream binary fixture helpers.
- `unit.c`: assertion-based equivalents of the legacy printed CMS unit/demo
  workloads, plus cell-size boundaries, rollback, weighted merges and overflow.
  The upstream demo references a commented-out CMS_Print and is not directly
  executable; it is not claimed as an unchanged port.
- `test_cms_native.py`: COPY, TYPE/SCAN, expiry, binary data, native memory,
  ACLs, RESP3, notifications, WATCH (upstream case), replication, both AOF
  rewrite formats, partial-error AOF replay, and external compatibility.

The stdlib harness is shared with `tests/bloom`; no RLTest or redis-py is needed.
Module-specific exact memory totals are replaced with allocation lower bounds
and relative cell-size checks. WATCH uses two real connections.
Set `BLOOM_ORACLE_SERVER` and `BLOOM_ORACLE_MODULE` to enable bidirectional
DUMP/RESTORE against the original module. Without them that case explicitly skips.
Native CMS defragmentation and cluster-mode execution need additional coverage.
