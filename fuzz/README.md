# Redis fuzzing

This directory contains opt-in libFuzzer targets for Redis core. The targets
exercise Redis commands through the real command parser and executor, using a
local socketpair-backed client.

The command-extension target covers `INCREX`, `SUNIONCARD`, `SDIFFCARD`, and
the `AGGREGATE COUNT` mode of sorted-set union/intersection commands. It
generates stateful option, encoding, aliasing, and wrong-type combinations.
Reserved keys provide exact post-execution oracles for integer saturation, set
cardinality, and weighted COUNT scores.

Its initial corpus also preserves minimized UBSan regressions for a relative
`INCREX` TTL overflow and malformed sorted-set key-count arithmetic.

The shared harness is data-structure agnostic. Feature branches can add focused
targets without changing normal Redis builds.

In CI, the fuzz campaigns run in the Daily workflow (`daily.yml`, on a cron
schedule or via manual dispatch with the `fuzz` token removed from `skipjobs`).
The per-PR CI (`ci.yml`) only runs a smoke job that checks seed drift, builds
the fuzz targets, and replays the checked-in corpora with `-runs=0`.

`fuzz_list_move_commands` is a stateful target for Redis 8.10 `LMOVEM` and
`BLMOVEM`. Its generated pipelines grow, shrink, rotate, delete, and change the
types of source and destination keys. It covers both directions, `COUNT` and
`EXACTLY`, `OBO` and `BULK`, missing and aliased keys, listpack/quicklist-sized
binary values, and malformed command shapes. A bounded in-memory list model is
checked directly against the Redis database, including move count, destination
ordering, same-key rotation, and the all-or-nothing behavior of `EXACTLY`. A
`BLMOVEM` with insufficient input registers the real blocked-client state; the
harness checks that state, then destroys the socket-backed client immediately,
exercising unblock and key-wait cleanup without waiting on wall-clock time.

`fuzz_replication_compression` exercises the client-level replication
compression implementation through real socket-backed clients. It independently
varies plaintext write chunks, compressor frame flushes, compressed input
chunks, and decompressed output buffer sizes. It checks exact round trips across
single, empty, duplicated, and concatenated Zstd frames, and feeds truncated,
corrupted, prefixed, suffixed, and fully arbitrary compressed streams through
the decompressor. Every input creates and destroys fresh compression and
decompression state to cover lifecycle cleanup.

The Array target generates bounded, stateful command sequences. It covers
contiguous and sparse writes, sparse-to-dense slice transitions, range deletion,
ring resize and wraparound, cursor movement, scanning, textual search,
reductions, wrong types, boundary indices, and malformed command shapes.

The targets are intentionally independent from native bitmap work. Once this
infrastructure lands upstream, native bitmap targets can extend it in the
feature branch. The stream target establishes consumer-group pending entries
before mutating `XNACK`, `XCLAIM`, `XACK`, `XREAD`, and `XREADGROUP` sequences,
including the `MAXCOUNT` and `MAXSIZE` reply limits.

`fuzz_backup_state_machine.py` is a process-level fault-injection target for
`BACKUP`. It starts isolated Redis processes, drives the real asynchronous
MP-AOF lifecycle, kills rewrite children, removes live INCR files, changes AOF
configuration, and validates artifacts, cleanup, transactions, and preload
recovery. Failures preserve the seed, server log, and scenario directory.

## Build

```sh
make fuzz CC=clang
```

The fuzz build uses libFuzzer with AddressSanitizer and UndefinedBehaviorSanitizer
and forces `MALLOC=libc`, matching Redis sanitizer builds. It also enables
replication compression with `BUILD_COMPRESSION=yes` and links libzstd. Normal
Redis builds are unchanged.

## Seed corpora

The checked-in seed files are binary inputs for the command generators, not RESP
payloads. Regenerate them with:

```sh
fuzz/generate-seeds.sh
```

## Run corpus smoke tests

```sh
fuzz/fuzz_string_commands fuzz/corpus/string_commands -runs=1
fuzz/fuzz_bitmap_commands fuzz/corpus/bitmap_commands -runs=1
fuzz/fuzz_command_extensions fuzz/corpus/command_extensions -runs=1
fuzz/fuzz_hash_templates fuzz/corpus/hash_templates -runs=1
fuzz/fuzz_array_commands fuzz/corpus/array_commands -runs=1
fuzz/fuzz_replication_compression fuzz/corpus/replication_compression -runs=1
fuzz/fuzz_list_move_commands fuzz/corpus/list_move_commands -runs=1
fuzz/fuzz_stream_commands fuzz/corpus/stream_commands -runs=1
```

## Run a short campaign

```sh
fuzz/fuzz_string_commands fuzz/corpus/string_commands -max_total_time=300
fuzz/fuzz_bitmap_commands fuzz/corpus/bitmap_commands -max_total_time=300
fuzz/fuzz_command_extensions fuzz/corpus/command_extensions -max_total_time=300
fuzz/fuzz_hash_templates fuzz/corpus/hash_templates -max_total_time=300
fuzz/fuzz_array_commands fuzz/corpus/array_commands -max_total_time=300
fuzz/fuzz_replication_compression fuzz/corpus/replication_compression -max_total_time=300
fuzz/fuzz_list_move_commands fuzz/corpus/list_move_commands -max_total_time=300
fuzz/fuzz_stream_commands fuzz/corpus/stream_commands -max_total_time=300
```

Run the BACKUP state-machine corpus and continue generating deterministic
random scenarios for five minutes. Build the standalone server with the same
ASan+UBSan instrumentation and test-only fault controls used by CI first:

```sh
make redis-server CC=clang SANITIZER=address MALLOC=libc \
  SKIP_VEC_SETS=yes OPTIMIZATION=-O1 -j"$(nproc)" \
  REDIS_CFLAGS='-DREDIS_TEST -fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' \
  REDIS_LDFLAGS='-fsanitize=undefined'
```

```sh
python3 fuzz/fuzz_backup_state_machine.py \
  --redis-server src/redis-server \
  --duration 300 \
  --artifact-dir fuzz-backup-artifacts
```

Reproduce a BACKUP failure using the seed recorded in
`fuzz-backup-artifacts/failure-*/reproducer.json`:

```sh
python3 fuzz/fuzz_backup_state_machine.py \
  --redis-server src/redis-server \
  --seed 0x0123456789abcdef
```

## Reproduce a crash

```sh
fuzz/fuzz_bitmap_commands crash-1234
fuzz/fuzz_bitmap_commands -minimize_crash=1 crash-1234
```

Minimized crash inputs should be added to the relevant corpus directory. If a
crash captures an important semantic regression, add a deterministic Redis test
for it as well.

## Compact hashes and `HIMPORT`

`fuzz_hash_templates` always prepares the same schema in two different field
orders and creates two keys backed by the shared template. It then mutates a
bounded model with `HIMPORT PREPARE`, `SET`, `DISCARD`, and `DISCARDALL`, ordinary
hash commands, `COPY`, `DUMP`, `RESET`, deletion, wrong-type values, and invalid
command shapes. The target varies listpack thresholds to exercise both
`template-listpack` and `template-array`, and checks the resulting database
against its in-memory field/value model after every generated command stream.
