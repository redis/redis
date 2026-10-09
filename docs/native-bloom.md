# Native Bloom filter migration

This experimental, opt-in slice makes Bloom a native Redis object (`OBJ_BLOOM`),
without module APIs or additional build dependencies. It is not yet a replacement
for the complete RedisBloom module.

## Organization

- `src/t_bloom.c`: commands and Redis object lifecycle integration.
- `src/bloom.c`, `src/bloom.h`: reusable scalable-filter algorithm interface.
- `src/bloom_filter.*`, `src/bloom_murmur.*`: filter and hashing internals.
- `src/bloom_rdb.c`: persistence and compatibility with RedisBloom.
- `src/commands/bf.*.json`: native command metadata.
- `tests/integration/bloom.py`: dependency-free Python regression tests.

Keeping commands separate from algorithms allows future core consumers to reuse
the implementation without invoking commands or the module API. File placement
does not change ownership: the data types team can maintain this code in `src/`.

## Build and run

```sh
make -C src -j4 redis-server redis-cli BUILD_BLOOM=yes
./src/redis-server
python3 tests/integration/bloom.py
```

Bloom is disabled by default. Only the Redis C toolchain and bundled dependencies
are required. Termux still requires the separate local core portability patches.
Do not load external RedisBloom in a Bloom-enabled server: the commands conflict.
In particular, do not use a configuration that loads `redisbloom.so`.

## Supported surface and compatibility

The first slice supports `BF.RESERVE`, `BF.ADD`, `BF.EXISTS`, `BF.SCANDUMP`, and
`BF.LOADCHUNK`, plus `bf-initial-size`, `bf-error-rate`, and `bf-expansion-factor`.
It preserves hashing, scaling, chunk format, and RESP2/RESP3 replies. `TYPE` now
returns `bloom`; `SCAN TYPE bloom`, COPY, expiry, memory accounting, lazy freeing,
digest, ACLs, notifications, and normal command propagation use core integration.

RDB deliberately retains the `MBbloom--` module wire ID and encoding version 4
for bidirectional migration, despite using a native object in memory. The loader
supports versions 0–4; historical fixtures still need coverage. AOF rewrite uses
`BF.LOADCHUNK` or an RDB preamble.

Set `BLOOM_ORACLE_SERVER` to a non-Bloom-enabled server and `BLOOM_ORACLE_MODULE`
to the RedisBloom shared library to test byte-identical chunks and bidirectional
DUMP/RESTORE. `REDIS_SERVER` overrides the tested executable.

## Remaining work

Complete the remaining BF commands, historical fixtures, and broader CI coverage
before enabling this by default. Defragmentation hooks need jemalloc validation;
large-chain incremental defragmentation remains follow-up work.

Algorithms derive from RedisBloom v8.11.81. The filter retains its BSD notice and
`src/bloom.LICENSE`; MurmurHash2 is public domain. Redis-authored code follows the
repository's RSALv2 / SSPLv1 / AGPLv3 licensing choices.
