# Native Bloom tests

These tests are ported from **RedisBloom v8.11.81**, using the core Bloom algorithm
and a native server. No external checkout, RLTest, redis-py, readies, or pip packages
are required. The original external module checkout is left intact for comparison.

## Run

Native Bloom and its tests are enabled by default; `BUILD_BLOOM=yes` below is
explicit but optional. `BUILD_BLOOM=no` disables native Bloom and this suite.

```sh
make -C src test-bloom BUILD_BLOOM=yes
BLOOM_LARGE_TESTS=1 make -C src test-bloom BUILD_BLOOM=yes
python3 tests/bloom/run.py test_configs test_corrupt_rdb
```

`make -C src test BUILD_BLOOM=yes` also runs this suite. Pass the same allocator
and compiler options used to build your server; on this Termux checkout these are
`MALLOC=libc CFLAGS= LDFLAGS= OPT=-O2` and the separate local portability patches.
`REDIS_SERVER` overrides the flow-test executable.

Large-memory cases include allocations over 1 GiB, large chunk transfers, and
million-item workloads. They remain available but are opt-in for smaller machines.
Defrag explicitly skips when the server lacks jemalloc support. The upstream
insufficient-memory flow case remains disabled: its capacity exceeds the command's
supported range. The upstream commented-out issue-6 unit regression is retained.

Set `BLOOM_ORACLE_SERVER` and `BLOOM_ORACLE_MODULE` to compare all commands in
RESP2/RESP3 against external RedisBloom, compare serialized chunks, and exercise
bidirectional DUMP/RESTORE. Without these settings only the oracle cases skip.

## Migration inventory

| Upstream source | Native destination and adaptations |
| --- | --- |
| `tests/unit/test-basic.c`, `test.h` | `unit.c`, `unit_support.h`: all nine active algorithm cases, direct core allocation; large cases opt-in |
| `tests/unit/test-perf.c` | `perf.c`: standalone algorithm benchmark, built with `make -C src bloom-perf BUILD_BLOOM=yes` |
| `tests/flow/test_overall.py` | `test_commands.py`: all Bloom cases, including both formerly duplicate large-filter methods |
| Bloom class in `test_bf_restore_corrupt_rdb.py` | `test_corrupt_rdb.py`: all three Bloom corruption cases and their helper functions |
| Bloom portions of `test_acl.py` | `test_acl.py`: category, command membership, user permissions, plus key-pattern checks |
| Bloom portions of `test_configs.py`, deprecated `init_test.py` | `test_configs.py`: defaults, changes, invalid values, startup options, debug statistics; native startup replaces module-load arguments |
| Bloom portion of `test_resp3.py` | `test_resp3.py`: boolean replies and INFO map |
| Bloom cases in `test_docs_help.py` | `test_docs.py`: all ten cases adapted to native metadata, plus DEBUG |
| Bloom portion of `test_defrag.py` | `test_defrag.py`: fragmentation/reclamation with Bloom keys and post-defrag membership checks |
| Bloom benchmark YAML | `benchmarks/bf_add_cap10M_err0.001.yml`: original workload specification |

`test_native.py` adds binary-safe parsing, partial-write/WATCH behavior, multi-write
AOF replay, RESP3 error arrays, and differential command checks.
`../integration/bloom.py` supplies replication, AOF rewrite, lifecycle, and migration
tests. Unit and flow failures make the runner exit nonzero.
`test_build_defaults.py` checks config generation, run selection, and deployment
in disposable fixtures so external RedisBloom is not auto-loaded by default.

Native-specific adaptations are intentional: module overhead is not part of
`MEMORY USAGE`, command docs use the native `bloom` group, and legacy module-load
aliases do not apply. The large unit test's floating-point assertion now checks
the actual halved first-link error rate instead of silently truncating to integers.
The other RedisBloom datatypes' tests remain out of scope. Original disabled code
stays recognizable; generator-based reload tests run through their final assertions.
