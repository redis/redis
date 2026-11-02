# Native Top-K tests

Run `make test-topk` with Python 3 and the ordinary Redis toolchain.
Upstream flow cases and the corrupt-RDB regression are retained; the upstream
print-only unit demo is replaced by assertion-based heap, controlled-count,
increment, distribution and decay workloads. Native cases check lifecycle,
RESP3, ACL, exact-state replication/AOF and real old-module restart upgrades.

`python3 tests/topk/run.py test_topk_chk` runs the CHK opt-in cases alone.
They exercise mixed-engine keys, configuration changes, both AOF rewrite modes,
historical command AOFs, mismatched replica defaults/full resync, binary names,
memory/lifecycle, corrupt persistence and 64-bit counters. Real module-upgrade
cases require `BLOOM_ORACLE_SERVER` and `BLOOM_ORACLE_MODULE`; all other CHK flow
cases run without external modules. Builds with `-DREDIS_TEST` also run the
core CHK algorithm unit tests through `make test-topk`.

See [native Top-K](../../docs/native-topk.md) for compatibility and limitations.
