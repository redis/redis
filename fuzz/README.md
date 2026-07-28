# Redis fuzzing

This directory contains opt-in libFuzzer targets for Redis core. The targets
exercise string, string-backed bitmap, and compact-hash commands through the
real command parser and executor, using a local socketpair-backed client.

The shared harness is data-structure agnostic. Feature branches can add focused
targets without changing normal Redis builds.

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
fuzz/fuzz_hash_templates fuzz/corpus/hash_templates -runs=1
```

## Run a short campaign

```sh
fuzz/fuzz_string_commands fuzz/corpus/string_commands -max_total_time=300
fuzz/fuzz_bitmap_commands fuzz/corpus/bitmap_commands -max_total_time=300
fuzz/fuzz_hash_templates fuzz/corpus/hash_templates -max_total_time=300
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
