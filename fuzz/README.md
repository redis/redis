# Redis fuzzing

This directory contains opt-in libFuzzer targets for Redis core. The command
targets exercise Redis string and string-backed bitmap commands through the real
command parser and executor, using a local socketpair-backed client.

`fuzz_replication_compression` exercises the client-level replication
compression implementation through real socket-backed clients. It independently
varies plaintext write chunks, compressor frame flushes, compressed input
chunks, and decompressed output buffer sizes. It checks exact round trips across
single, empty, duplicated, and concatenated Zstd frames, and feeds truncated,
corrupted, prefixed, suffixed, and fully arbitrary compressed streams through
the decompressor. Every input creates and destroys fresh compression and
decompression state to cover lifecycle cleanup.

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
fuzz/fuzz_replication_compression fuzz/corpus/replication_compression -runs=1
```

## Run a short campaign

```sh
fuzz/fuzz_string_commands fuzz/corpus/string_commands -max_total_time=300
fuzz/fuzz_bitmap_commands fuzz/corpus/bitmap_commands -max_total_time=300
fuzz/fuzz_replication_compression fuzz/corpus/replication_compression -max_total_time=300
```

## Reproduce a crash

```sh
fuzz/fuzz_bitmap_commands crash-1234
fuzz/fuzz_bitmap_commands -minimize_crash=1 crash-1234
```

Minimized crash inputs should be added to the relevant corpus directory. If a
crash captures an important semantic regression, add a deterministic Redis test
for it as well.
