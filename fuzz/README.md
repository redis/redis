# Redis fuzzing

This directory contains opt-in libFuzzer targets for Redis core. The targets
exercise Redis string, string-backed bitmap, and focused command extensions
through the real command parser and executor, using a local socketpair-backed
client.

The command-extension target covers `INCREX`, `SUNIONCARD`, `SDIFFCARD`, and
the `AGGREGATE COUNT` mode of sorted-set union/intersection commands. It
generates stateful option, encoding, aliasing, and wrong-type combinations.
Reserved keys provide exact post-execution oracles for integer saturation, set
cardinality, and weighted COUNT scores.

Its initial corpus also preserves two minimized UBSan findings: a relative
`INCREX` TTL overflow and malformed sorted-set key-count arithmetic. The draft
command-extension CI job is expected to remain red on those seeds until the
corresponding production fixes land.

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
fuzz/fuzz_command_extensions fuzz/corpus/command_extensions -runs=1
```

## Run a short campaign

```sh
fuzz/fuzz_string_commands fuzz/corpus/string_commands -max_total_time=300
fuzz/fuzz_bitmap_commands fuzz/corpus/bitmap_commands -max_total_time=300
fuzz/fuzz_command_extensions fuzz/corpus/command_extensions -max_total_time=300
```

## Reproduce a crash

```sh
fuzz/fuzz_bitmap_commands crash-1234
fuzz/fuzz_bitmap_commands -minimize_crash=1 crash-1234
```

Minimized crash inputs should be added to the relevant corpus directory. If a
crash captures an important semantic regression, add a deterministic Redis test
for it as well.
