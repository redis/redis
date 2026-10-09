# Built-in Bloom filter migration

This experimental, opt-in implementation links the Bloom filter slice of
RedisBloom into redis-server using the internal-module infrastructure also
used by Vector Set. It is a first migration milestone: it retains the module
API and `OBJ_MODULE` representation, rather than adding a native object type.

## Build and run

```sh
make -C src -j4 redis-server redis-cli BUILD_BLOOM=yes
./src/redis-server
```

Only the existing Redis C toolchain and bundled dependencies are required.
No external module checkout, Rust, CMake, readies, Modules SDK, or BlocksRuntime
is needed. On Termux, the existing local core portability patches are still
required; they are separate from this feature.

Bloom is disabled by default. Do not load external RedisBloom into a server
built with `BUILD_BLOOM=yes`: they own the same commands and data type. In
particular, an existing `redis-full.conf` with `loadmodule .../redisbloom.so`
must not be used for this experimental build. Other RedisBloom data structures
are not provided by the built-in slice.

## Supported surface

- `BF.RESERVE`, `BF.ADD`, and `BF.EXISTS` retain RedisBloom hashing, scaling,
  default settings, and RESP2 integer / RESP3 boolean responses.
- `BF.SCANDUMP` and `BF.LOADCHUNK` support migration and AOF rewrite.
- `bf-initial-size`, `bf-error-rate`, and `bf-expansion-factor` configure defaults.
- RDB uses `MBbloom--`, encoding version 4. The loader handles older layouts
  from the source implementation; historical fixtures still need coverage.
- Normal command propagation supports replication and AOF. Memory accounting
  and ACL categories are registered through the module API.

## Verification

```sh
python3 modules/bloom/test.py
```

Tests use Python's standard library and disposable servers, with no pip packages.
They exercise persistence, scaling, binary values, ACLs, RESP3, and replication.
Set `BLOOM_ORACLE_SERVER` to a server built without built-in Bloom and
`BLOOM_ORACLE_MODULE` to an external RedisBloom shared library to enable
byte-for-byte chunk comparison and bidirectional DUMP/RESTORE tests.
`REDIS_SERVER` overrides the binary under test.

## Follow-up migration work

Complete the remaining BF commands and historical-format tests before replacing
the external module. Moving command handlers, allocation, and object lifecycle
to native core APIs remains a separate step. Active defragmentation, COPY, and
digest callbacks are not implemented in this milestone.

Algorithm sources were adapted from RedisBloom v8.11.81: `src/sb.*`,
`deps/bloom/bloom.*`, and `deps/murmur2/*`. Bloom algorithm code retains its BSD
notice and accompanying LICENSE; MurmurHash2 is public domain. Redis-authored
code follows the repository's RSALv2 / SSPLv1 / AGPLv3 licensing choices.
