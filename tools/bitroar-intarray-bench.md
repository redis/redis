# Integer-array command benchmarks

`bitroar-intarray-bench.py` measures `R.GETINTARRAY` and `R.RANGEINTARRAY` against
isolated Redis processes over Unix sockets. It needs Python 3.9+ and a normal
optimized Redis build; no Python packages or existing server are required.

```sh
make -j4
python3 tools/bitroar-intarray-bench.py --json-out /tmp/intarray-native.json
```

The optional redis-roaring module baseline needs an **unmodified base Redis
build**, without the new native `R.*` command names, in a second isolated
process. Renaming or disabling the native names is insufficient: module
registration also checks Redis's original command table. Build the parent of
the command change with the same compiler and flags, and pass its binaries via
`--module-src-dir`. Pin and record the module revision:

```sh
git clone https://github.com/aviggiano/redis-roaring.git /tmp/redis-roaring
git -C /tmp/redis-roaring checkout e7382b7cea1ec495304e8aa60916c49673e74c1d
git -C /tmp/redis-roaring submodule update --init deps/CRoaring deps/hiredis
(cd /tmp/redis-roaring/src && ../deps/CRoaring/amalgamation.sh)
cmake -S /tmp/redis-roaring -B /tmp/redis-roaring-build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/redis-roaring-build --target redis-roaring -j4
python3 tools/bitroar-intarray-bench.py \
  --module /tmp/redis-roaring-build/libredis-roaring.so \
  --module-src-dir /path/to/unmodified-base/src \
  --module-revision e7382b7cea1ec495304e8aa60916c49673e74c1d \
  --json-out /tmp/intarray-comparison.json
```

With CMake 4, older dependency minimum versions may require adding
`-DCMAKE_POLICY_VERSION_MINIMUM=3.5` to the configure command.

The deterministic datasets include missing keys, dense consecutive runs,
alternating-bit bitsets, sparse values across 16-bit containers, cardinalities
16/4096/65536, and sparse values above 2^40. Module comparisons use its `R.*`
commands for 32-bit values and `R64.*` for the high-offset dataset. The latter is
an explicit comparison against the module's 64-bit API, since `R.*` cannot
represent those values.

Every reply is checked against the expected ascending values before timing.
Ranges are **inclusive element ranks**, not value intervals: first 16 elements,
a middle page containing 1% of the elements, last 16 elements, the whole array,
and an empty page beyond the cardinality. The same logical data is prepared
and run-optimized on each backend outside timing. Native setup uses SETBIT and
a DUMP/RESTORE round trip; module setup uses SETINTARRAY and OPTIMIZE MEM.

Each workload is warmed up and measured three times, alternating backend order.
The report includes raw redis-benchmark output, every sample, and the median,
mean, sample standard deviation, minimum and maximum for throughput,
redis-benchmark latency percentiles and server command execution microseconds.
It also records returned element
count, and exact RESP2 reply bytes. Request counts are capped by an approximate
32 MiB reply budget and 0.5-second time budget per trial, with a floor of one
request. The slower backend's initial complete-response validation call bounds
warmup to approximately 0.25 seconds or 100 requests. The slower measured warmup
throughput then determines the timed request count; both backends use that same
count. Validation includes Python response parsing and only provides a fallback
when warmup throughput is invalid or zero (this client reports zero when its
millisecond timer cannot resolve the elapsed time). Positive infinity is also
handled defensively by retaining only the reply/count caps. Warmup output and
validation times are retained in the JSON. Small
replies use up to 5000 requests. Change `--requests`, `--reply-budget-mib`,
`--trial-seconds`, and `--trials` for longer runs; use `--dataset sparse-65536`
to select a subset. The JSON records the actual counts. Extremely short trials
can fall below redis-benchmark's millisecond throughput timer resolution;
server command execution time remains reported separately.

Default concurrency and pipeline depth are both one. They can be changed with
`--clients` and `--pipeline`; compare runs with identical settings. Request
counts are divisible by the pipeline depth, with at least one full batch even
when that exceeds the nominal budget. The JSON
records CPU affinity, CPU model, cgroup CPU quota, platform, binary hashes,
module revision, and Redis version. Run on an idle machine and retain the JSON
with the source revision and compiler/build options used.

Throughput includes consuming the complete response with redis-benchmark's C
client. Its latency measurements stop on the first socket read and therefore
must not be described as full-array end-to-end latency. Server execution time
comes from INFO commandstats deltas and excludes transport/client parsing.
Large-array throughput naturally includes reply serialization, buffering, and
network/client costs; small-page throughput can be limited by round-trip
overhead. These measurements are diagnostic results, not CI pass/fail gates.

## Measured comparison (2026-10-15)

This run measured all 50 workloads against both implementations, with three
trials per backend. Full replies and exact command counts passed validation.
Results below describe this machine and these optimized, preloaded datasets;
they are not a general speedup claim.

The native tested source was `7b44c6a975cae96eb806099f99165f63a2489d24`;
the unmodified baseline Redis source was
`4e4c65bc71dd63c1d2dbe999b3d3d86d49772cc5`. Both used GCC 14.2.0,
`BUILD_TLS=yes REDIS_CFLAGS=-Werror`, Redis's default `-O3`/LTO optimization,
and jemalloc 5.3.0. The pinned module was
`e7382b7cea1ec495304e8aa60916c49673e74c1d`, built with CMake Release and
`ROARING_BUILD_STATIC=ON`. Its CRoaring dependency was
`baa32837f9f693aee54aeec97051b54ee522e21a` and hiredis was
`60e5075d4ac77424809f855ba3e398df7aacefe8`.

The host reported `Linux-6.18.44-x86_64-with-glibc2.41`, Python 3.12.14, and
`INTEL(R) XEON(R) PLATINUM 8573C`. All benchmark processes inherited affinity to
CPUs 0 and 1; the container's CPU quota was four CPUs (`400000 100000`). No
other builds or tests were running. This remains a virtualized shared host:
scheduling and CPU-frequency variation affect the samples. Each server used a
Unix socket, persistence disabled, one client, and pipeline depth one. The run
started at `2026-10-15T18:58:24Z`.

The native binary was built from the tested source before its commit, so its
embedded version says `sha=6ee76514:1`. The recorded source revision and binary
hash below identify what was measured. The baseline embedded version says
`sha=4e4c65bc:0`.

| Artifact | SHA-256 |
| --- | --- |
| Native redis-server | `9de45976ba8cde3efdc021e4ab5486b78c4c60e6f86d29c77ae20c3b774aae9d` |
| Baseline redis-server | `03e9e9309e76d3cfae162a532e77eb08c87d1d0647f2986f22abdf09d4a20b80` |
| Shared redis-benchmark client | `0bab90cf0f3fce55c01b7e755cd865ad3ab3601863973fe5784bd6d51d9efac2` |
| Reference module | `ce9182a912bce173aaa58c6bbe0164f8598ae8e537d78d402e8bf5fa440b1d66` |
| Raw result JSON | `edc7ea96e7741c59f5105862f61780c7421bf3ff323dac56aeb8690cf98eb857` |

The exact invocation, from the native checkout, was:

```sh
taskset -c 0,1 python3 tools/bitroar-intarray-bench.py \
  --module /workspace/scratch/intarray-module-build/libredis-roaring.so \
  --module-src-dir /workspace/redis-intarray-baseline/src \
  --module-revision e7382b7cea1ec495304e8aa60916c49673e74c1d \
  --requests 20000 --trials 3 \
  --json-out /workspace/scratch/bitroar-intarray-comparison.json \
  > /workspace/scratch/bitroar-intarray-comparison.log 2>&1
```

The raw JSON retains all samples, warmups, request counts, exact reply sizes,
latency metrics, build settings, and binary hashes. The JSON and stdout log are
local review artifacts at the paths above, not files in the repository.

The main observations, using median server execution time, are:

- Sparse 65,536-element `RANGEINTARRAY` first-page time is
  2.30 µs. Before removing the up-front
  cardinality scan it was 595.18 µs
  (about 259× higher).
  The earlier run used 310 requests per trial; this run uses
  5000, with improved warmup calibration.
  This diagnostic comparison motivated the change; it is not a controlled
  cross-version throughput claim. The module's first page is
  2.87 µs in this run.
- Sparse 65,536-element middle and last pages take
  640.80 and
  900.28 µs natively, versus
  61,807.50 and
  3,604.69 µs in the module.
  Returning the entire sparse array through RANGE takes
  5.06 ms natively versus
  6.79 seconds in the module.
  The module selects each requested rank separately; the native command skips
  to the first rank and then iterates forward.
- Native sparse `GETINTARRAY` remains slower: the 65,536-element case takes
  3.86 ms versus
  2.12 ms. The empty page after
  that bitmap's end takes 641.59 µs natively
  versus 172.93 µs. Traversing to a
  distant rank still costs time even when no values are returned.
- High-offset results use the module's R64 API. They demonstrate the native
  commands' 64-bit coverage, and should not be labeled a comparison against
  the module's 32-bit R API. Small-page throughput often reflects scheduling
  and round-trip overhead more than server execution time. The variance and
  full matrix below should guide interpretation.

The very slow sparse RANGE-all case uses one request per trial, so each
backend has only three single-command samples there. This bounds runtime but
makes its native throughput estimate especially coarse. Other large replies
are limited by the 32 MiB response budget. Three trials are diagnostic,
not a statistical confidence interval; the sample standard deviations below
expose variability rather than establish significance. The earlier implementation
run is preserved separately as `bitroar-intarray-pre-optimization.json`; the
intermediate run before improving calibration is
`bitroar-intarray-cold-calibrated.json`, both in `/workspace/scratch`.

In the table, `n` is requests per trial for **each** backend, and `ints` is
returned integers per request. Server times are microseconds, shown as
**mean ± sample standard deviation (median)**. Throughput is median complete
responses per second. All 50 workloads are included; module times in the
`high64-4096` rows use R64 commands.

| Dataset / workload | n | ints | Native server µs | Module server µs | Native req/s | Module req/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| empty / get-all | 20,000 | 0 | 0.18 ± 0.12 (0.12) | 0.39 ± 0.08 (0.37) | 67,114.09 | 22,522.52 |
| empty / range-empty | 7,142 | 0 | 0.16 ± 0.07 (0.16) | 0.54 ± 0.08 (0.51) | 41,766.08 | 12,596.12 |
| dense-16 / get-all | 12,500 | 16 | 0.93 ± 0.11 (0.92) | 1.62 ± 0.49 (1.37) | 14,654.16 | 18,037.52 |
| dense-16 / range-first16 | 6,000 | 16 | 1.46 ± 0.10 (1.49) | 1.76 ± 0.27 (1.81) | 11,342.16 | 16,759.78 |
| dense-16 / range-middle1pct | 4,545 | 1 | 1.00 ± 0.30 (0.85) | 0.98 ± 1.08 (0.39) | 15,000.00 | 126,250.01 |
| dense-16 / range-last16 | 12,500 | 16 | 1.16 ± 0.16 (1.07) | 1.12 ± 0.23 (1.13) | 18,409.43 | 27,777.78 |
| dense-16 / range-all | 4,545 | 16 | 1.34 ± 0.29 (1.41) | 1.93 ± 0.54 (1.84) | 11,713.92 | 12,055.70 |
| dense-16 / range-empty | 4,545 | 0 | 0.59 ± 0.21 (0.47) | 1.08 ± 0.06 (1.07) | 20,659.09 | 22,725.00 |
| sparse-16 / get-all | 4,166 | 16 | 2.02 ± 0.36 (1.90) | 2.10 ± 0.13 (2.12) | 10,311.88 | 10,467.34 |
| sparse-16 / range-first16 | 3,333 | 16 | 1.78 ± 0.38 (1.59) | 1.86 ± 0.07 (1.84) | 10,255.38 | 11,000.00 |
| sparse-16 / range-middle1pct | 10,000 | 1 | 1.00 ± 0.04 (1.00) | 1.06 ± 0.05 (1.09) | 21,505.38 | 19,305.02 |
| sparse-16 / range-last16 | 6,250 | 16 | 1.93 ± 0.26 (2.03) | 2.13 ± 0.36 (2.08) | 8,445.95 | 11,425.96 |
| sparse-16 / range-all | 5,555 | 16 | 2.23 ± 0.15 (2.16) | 2.46 ± 0.51 (2.73) | 7,662.07 | 9,694.59 |
| sparse-16 / range-empty | 10,000 | 0 | 0.93 ± 0.18 (0.91) | 1.20 ± 0.12 (1.23) | 12,738.85 | 15,673.98 |
| dense-4096 / get-all | 1,217 | 4,096 | 69.58 ± 7.58 (65.74) | 75.41 ± 5.06 (73.03) | 2,212.73 | 2,035.12 |
| dense-4096 / range-first16 | 3,333 | 16 | 1.26 ± 0.19 (1.17) | 1.65 ± 0.42 (1.48) | 8,771.05 | 20,962.27 |
| dense-4096 / range-middle1pct | 10,000 | 40 | 1.90 ± 0.04 (1.90) | 2.17 ± 0.04 (2.16) | 14,224.75 | 17,241.38 |
| dense-4096 / range-last16 | 6,250 | 16 | 1.43 ± 0.16 (1.46) | 1.56 ± 0.55 (1.52) | 16,149.87 | 9,204.71 |
| dense-4096 / range-all | 980 | 4,096 | 75.89 ± 5.77 (76.18) | 113.44 ± 19.28 (109.67) | 1,519.38 | 1,462.69 |
| dense-4096 / range-empty | 4,166 | 0 | 0.77 ± 0.21 (0.80) | 0.86 ± 0.77 (0.42) | 8,807.61 | 99,190.48 |
| sparse-4096 / get-all | 707 | 4,096 | 235.58 ± 29.87 (224.11) | 128.26 ± 1.04 (128.32) | 1,370.16 | 1,716.02 |
| sparse-4096 / range-first16 | 5,555 | 16 | 3.09 ± 0.82 (3.55) | 2.78 ± 0.06 (2.78) | 13,226.19 | 10,268.02 |
| sparse-4096 / range-middle1pct | 2,000 | 40 | 30.53 ± 5.64 (28.01) | 246.84 ± 39.70 (225.79) | 7,604.56 | 1,964.64 |
| sparse-4096 / range-last16 | 1,282 | 16 | 42.41 ± 1.12 (42.32) | 211.27 ± 12.30 (214.55) | 19,134.33 | 2,143.81 |
| sparse-4096 / range-all | 25 | 4,096 | 254.59 ± 72.52 (269.76) | 23,600.52 ± 1,959.07 (24,525.28) | 1,562.50 | 40.06 |
| sparse-4096 / range-empty | 2,941 | 0 | 50.57 ± 5.96 (49.44) | 14.45 ± 3.02 (13.07) | 9,306.96 | 8,939.21 |
| dense-65536 / get-all | 58 | 65,536 | 1,414.02 ± 88.74 (1,431.50) | 1,443.66 ± 146.34 (1,398.02) | 133.03 | 176.83 |
| dense-65536 / range-first16 | 5,000 | 16 | 1.78 ± 0.47 (1.66) | 1.54 ± 0.63 (1.47) | 8,726.00 | 18,315.02 |
| dense-65536 / range-middle1pct | 1,923 | 655 | 25.37 ± 10.59 (25.68) | 18.12 ± 0.35 (18.13) | 6,771.13 | 15,892.56 |
| dense-65536 / range-last16 | 12,500 | 16 | 2.08 ± 0.36 (2.16) | 1.03 ± 0.27 (0.90) | 11,415.52 | 99,206.34 |
| dense-65536 / range-all | 55 | 65,536 | 1,429.19 ± 212.79 (1,405.24) | 1,913.15 ± 295.61 (1,769.47) | 125.00 | 157.59 |
| dense-65536 / range-empty | 7,142 | 0 | 0.91 ± 0.17 (0.94) | 1.97 ± 1.75 (1.78) | 10,144.89 | 13,349.53 |
| sparse-65536 / get-all | 40 | 65,536 | 3,944.09 ± 440.02 (3,857.20) | 2,585.43 ± 817.58 (2,117.68) | 101.78 | 116.96 |
| sparse-65536 / range-first16 | 5,000 | 16 | 2.28 ± 0.27 (2.30) | 2.66 ± 0.41 (2.87) | 13,927.58 | 10,869.57 |
| sparse-65536 / range-middle1pct | 10 | 655 | 671.73 ± 357.60 (640.80) | 70,637.87 ± 16,415.90 (61,807.50) | 1,428.57 | 16.05 |
| sparse-65536 / range-last16 | 169 | 16 | 900.62 ± 205.02 (900.28) | 3,641.74 ± 576.90 (3,604.69) | 1,076.43 | 208.13 |
| sparse-65536 / range-all | 1 | 65,536 | 4,690.33 ± 725.29 (5,056.00) | 6,450,050.00 ± 860,410.60 (6,790,363.00) | 90.91 | 0.15 |
| sparse-65536 / range-empty | 641 | 0 | 643.46 ± 3.74 (641.59) | 173.07 ± 1.85 (172.93) | 959.58 | 5,297.52 |
| bitset-65536 / get-all | 38 | 65,536 | 1,390.46 ± 121.46 (1,396.53) | 1,391.28 ± 126.03 (1,380.71) | 156.38 | 143.94 |
| bitset-65536 / range-first16 | 7,142 | 16 | 1.64 ± 0.08 (1.65) | 1.27 ± 0.56 (1.05) | 15,766.00 | 66,129.62 |
| bitset-65536 / range-middle1pct | 3,550 | 655 | 17.25 ± 2.60 (18.03) | 47.56 ± 1.96 (48.27) | 7,717.39 | 3,629.86 |
| bitset-65536 / range-last16 | 2,272 | 16 | 5.45 ± 1.90 (5.15) | 32.03 ± 0.48 (32.02) | 7,624.16 | 24,170.21 |
| bitset-65536 / range-all | 6 | 65,536 | 2,177.61 ± 206.35 (2,236.67) | 103,734.72 ± 13,099.46 (102,373.83) | 88.24 | 9.06 |
| bitset-65536 / range-empty | 20,000 | 0 | 0.38 ± 0.10 (0.37) | 1.54 ± 0.82 (1.88) | 43,196.54 | 13,577.73 |
| high64-4096 / get-all | 511 | 4,096 | 301.96 ± 80.29 (286.76) | 325.60 ± 23.66 (330.44) | 1,296.95 | 1,327.27 |
| high64-4096 / range-first16 | 12,500 | 16 | 1.64 ± 0.40 (1.70) | 3.90 ± 0.30 (4.04) | 37,313.43 | 25,987.53 |
| high64-4096 / range-middle1pct | 588 | 40 | 26.44 ± 3.07 (26.62) | 868.15 ± 46.39 (847.25) | 14,000.00 | 1,157.48 |
| high64-4096 / range-last16 | 724 | 16 | 53.14 ± 11.44 (52.10) | 738.26 ± 67.57 (708.62) | 13,660.38 | 1,304.50 |
| high64-4096 / range-all | 5 | 4,096 | 260.27 ± 79.58 (256.20) | 99,447.67 ± 24,630.55 (87,323.20) | 714.29 | 11.21 |
| high64-4096 / range-empty | 7,142 | 0 | 56.26 ± 5.75 (56.05) | 43.60 ± 1.57 (43.22) | 9,251.29 | 19,302.70 |
