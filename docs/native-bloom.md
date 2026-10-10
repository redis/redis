# Native Bloom filter migration

This implementation makes Bloom a native Redis object (`OBJ_BLOOM`),
without module APIs or additional build dependencies. The full stack migrates
all five RedisBloom families; see [upstream provenance](probabilistic-upstream.md)
for the audited RedisBloom master revision.

## Organization

- `src/t_bloom.c`: commands and Redis object lifecycle integration.
- `src/bloom.c`, `src/bloom.h`: reusable scalable-filter algorithm interface.
- `src/bloom_filter.*`, `src/bloom_murmur.*`: filter and hashing internals.
- `src/bloom_rdb.c`: persistence and compatibility with RedisBloom.
- `src/commands/bf.*.json`: native command metadata.
- `tests/integration/bloom.py`: dependency-free Python regression tests.
- `tests/bloom/`: migrated Bloom unit and flow tests; see its README for provenance.

Keeping commands separate from algorithms allows future core consumers to reuse
the implementation without invoking commands or the module API. File placement
does not change ownership: the data types team can maintain this code in `src/`.

## Build and run

```sh
make -C src -j4 redis-server redis-cli
./src/redis-server
make -C src test-bloom
```

Bloom is enabled by default (`BUILD_BLOOM=yes`). Only the Redis C toolchain and bundled dependencies
are required. Termux still requires the separate local core portability patches.
Do not load external RedisBloom in a Bloom-enabled server: the commands conflict.
The run, generated-configuration, and deployment flows omit `redisbloom.so` and
its module-only settings. All five probabilistic families are now native;
RedisBloom has been removed from the bundled-module manifest.
Top-K is now native; see [its migration guide](native-topk.md).
Cuckoo is now native; see [its migration guide](native-cuckoo.md).
Count-Min Sketch is now native too; see [its migration guide](native-cms.md).
Existing manually maintained configs must remove any external RedisBloom
`loadmodule` directive. See [existing-key upgrades](probabilistic-upgrade.md).
`BUILD_BLOOM=no` disables the native family; it no longer selects a bundled module.

## Supported surface and compatibility

All eleven Bloom commands are supported: `BF.RESERVE`, `BF.ADD`, `BF.MADD`,
`BF.INSERT`, `BF.EXISTS`, `BF.MEXISTS`, `BF.INFO`, `BF.CARD`, `BF.DEBUG`,
`BF.SCANDUMP`, and `BF.LOADCHUNK`, plus `bf-initial-size`, `bf-error-rate`, and
`bf-expansion-factor`. Other RedisBloom datatypes are not included.
It preserves hashing, scaling, chunk format, and RESP2/RESP3 replies. `TYPE` now
returns `bloom`; `SCAN TYPE bloom`, COPY, expiry, memory accounting, lazy freeing,
digest, ACLs, notifications, and normal command propagation use core integration.
Multi-item writes preserve partial results on a full non-scaling filter.
`BF.INFO SIZE` retains the portable logical size used by RedisBloom, while
`MEMORY USAGE` reports native allocator accounting. `BF.INSERT` requires complete,
binary-safe option names rather than upstream's undocumented prefix matching.

RDB deliberately retains the `MBbloom--` module wire ID and encoding version 4
for bidirectional migration, despite using a native object in memory. The loader
supports versions 0–4; historical fixtures still need coverage. AOF rewrite uses
`BF.LOADCHUNK` or an RDB preamble.

Set `BLOOM_ORACLE_SERVER` to a non-Bloom-enabled server and `BLOOM_ORACLE_MODULE`
to the RedisBloom shared library to test byte-identical chunks and bidirectional
DUMP/RESTORE. `REDIS_SERVER` overrides the tested executable.

## Remaining work

Historical fixtures and broader CI coverage remain follow-up work.
Defragmentation hooks need jemalloc validation;
large-chain incremental defragmentation remains follow-up work.

Algorithms derive from RedisBloom v8.11.81. The filter retains its BSD notice and
`src/bloom.LICENSE`; MurmurHash2 is public domain. Redis-authored code follows the
repository's RSALv2 / SSPLv1 / AGPLv3 licensing choices.
