# Redis fuzzing

This directory contains opt-in libFuzzer targets for Redis core. The initial
targets exercise Redis string and string-backed bitmap commands through the real
command parser and executor, using a local socketpair-backed client.

The targets are intentionally independent from native bitmap work. Once this
infrastructure lands upstream, native bitmap targets can extend it in the
feature branch.

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
and forces `MALLOC=libc`, matching Redis sanitizer builds. Normal Redis builds are
unchanged.

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
```

## Run a short campaign

```sh
fuzz/fuzz_string_commands fuzz/corpus/string_commands -max_total_time=300
fuzz/fuzz_bitmap_commands fuzz/corpus/bitmap_commands -max_total_time=300
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
