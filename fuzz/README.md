# Redis fuzzing

This directory contains opt-in libFuzzer targets for Redis core. The targets
exercise Redis commands through the real command parser and executor, using a
local socketpair-backed client.

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
fuzz/fuzz_list_move_commands fuzz/corpus/list_move_commands -runs=1
```

## Run a short campaign

```sh
fuzz/fuzz_string_commands fuzz/corpus/string_commands -max_total_time=300
fuzz/fuzz_bitmap_commands fuzz/corpus/bitmap_commands -max_total_time=300
fuzz/fuzz_list_move_commands fuzz/corpus/list_move_commands -max_total_time=300
```

## Reproduce a crash

```sh
fuzz/fuzz_bitmap_commands crash-1234
fuzz/fuzz_bitmap_commands -minimize_crash=1 crash-1234
```

Minimized crash inputs should be added to the relevant corpus directory. If a
crash captures an important semantic regression, add a deterministic Redis test
for it as well.
