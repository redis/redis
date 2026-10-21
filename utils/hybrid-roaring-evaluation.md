# Hybrid Roaring set experiment

## Scope

The candidate is on `codex/hybrid-roaring-set-eval`, based on `unstable` commit
`b540ca49cba815f3fbe634363c3df68d4f4f127a`. The clean baseline worktree uses
the same commit. The feature is controlled by the immutable startup option
`set-roaring-enabled`, which defaults to `no`. Integer-only sets stay in intset
through `set-max-intset-entries` (512 by default), then use per-group Roaring32
when enabled; otherwise they use the existing hashtable. Small mixed sets
remain listpack, and larger mixed sets keep the existing hybrid encoding:
per-group Roaring32 for canonical signed 64-bit integers plus a dictionary for
other strings.

The signed-to-unsigned mapping flips the sign bit, preserving the full signed
64-bit range. Non-canonical numeric strings such as `01` remain distinct string
members. Sparse integers are grouped by their upper 48 bits; each group keeps
its lower 16 bits in a compact vector until it reaches the promotion threshold
(64), then only that group moves into a Roaring32 bitmap. Promotion is one-way for this
experiment, so removing members cannot cause promotion/demotion churn. RDB and
AOF keep the standard set wire format; on RDB load, a mixed hashtable may be
converted back to the experimental encoding. Key TTL is kept by the ordinary
keyspace path.

## Pure-integer follow-up

The initial experiment focused on mixed sets. This follow-up keeps that path
unchanged and tests the clarified design: a pure integer set remains intset up
to the configured limit, then converts to Roaring when
`set-roaring-enabled=yes`. With the default limit, both builds use intset at
512 members; at 513 members the baseline uses a hashtable and the candidate
uses `hybrid-roaring`.

The benchmark ran three alternating baseline/candidate rounds on the same
local arm64 machine, using the clean baseline and candidate built from
`b540ca49cba815f3fbe634363c3df68d4f4f127a`. Results below are medians across
the three runs; add and membership are operations/second, and algebra values
are microseconds per store operation.

| Size and pattern | Memory/member, baseline → Roaring | Add throughput | Membership throughput | `SUNIONSTORE` | `SDIFFSTORE` | `SINTERSTORE` |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 513 dense | 36.9 → 19.8 B | 0.93M → 0.94M/s | 1.04M → 0.96M/s | 131 → 99 | 82 → 81 | 75 → 72 |
| 513 sparse | 42.9 → 80.4 B | 1.00M → 0.98M/s | 1.05M → 1.03M/s | 138 → 167 | 94 → 86 | 76 → 67 |
| 32,768 dense | 30.7 → 0.6 B | 1.39M → 1.44M/s | 1.30M → 1.32M/s | 2,739 → 636 | 2,191 → 545 | 1,621 → 401 |
| 32,768 sparse | 36.7 → 48.3 B | 1.40M → 1.34M/s | 1.30M → 1.29M/s | 2,759 → 2,812 | 2,232 → 2,192 | 1,717 → 1,661 |
| 100,000 dense | 38.6 → 0.3 B | 1.35M → 1.45M/s | 1.20M → 1.33M/s | 9,406 → 1,780 | 6,831 → 1,520 | 5,405 → 1,047 |
| 100,000 sparse | 44.6 → 50.6 B | 1.33M → 1.33M/s | 1.25M → 1.27M/s | 9,685 → 9,726 | 7,062 → 7,018 | 5,751 → 5,405 |

Dense runs compress especially well because CRoaring stores consecutive
integers as runs. At 32,768 dense values the candidate uses about 98% less
memory and runs the three algebra stores 4–5 times faster, while add and
membership throughput stay close. Sparse singleton-heavy groups use more
memory: 31–87% more at the larger cardinalities measured, with generally
similar add and lookup rates and mixed algebra results. The 513-member sparse
case is a particularly poor crossover point because each group pays dictionary
and bucket overhead before enough values accumulate to promote.

The full raw matrix, including 512/513 boundary runs, negative values, and
high-magnitude values, is in `utils/pure-integer-roaring-results.csv`; the
repeatable command is in `utils/benchmark-pure-integer-roaring.py`. This is a
local synthetic benchmark, not an AWS run or a production workload sample.

## Validation

- Candidate and baseline built on arm64 with Apple clang 21.0.3, libc allocator,
  `-O3`, and LTO.
- `make -C src -j4`: passed on the candidate.
- `./runtest --single unit/type/set`: all functional set tests and macOS leak
  checks passed. The leak was a temporary string SDS left allocated when a
  duplicate member was rejected; the duplicate path now frees it.
- Focused hybrid semantics and RDB reload test: passed. It covers insertion
  order, signed 64-bit boundaries, `01` versus `1`, membership, copy, set
  intersection/store, removal, and key TTL preservation through reload.
- Manual AOF rewrite/restart with TTL: passed.
- Manual replica full sync and subsequent command propagation: passed.
- The broader benchmark exercises cardinalities 4,096 and 32,768; integer ratios
  90%, 50%, and 10%; and dense, consecutive-negative, sparse, spaced-negative,
  and high
  magnitude values. It reports memory per member, pipelined add/remove/membership
  rates, iteration throughput, set union/difference/intersection store latency, and single-command
  membership median/p95 latency. Final threshold-64 results are in
  `utils/hybrid-roaring-final-results.csv`.

## Results at 32,768 members, 90% integers

| Integer pattern | Baseline memory/member | Candidate memory/member | Load add ops/s, baseline → candidate | Readout |
| --- | ---: | ---: | ---: | --- |
| Consecutive positive | 31.7 B | 5.1 B | 1.22M → 1.26M | 84% less memory; writes and membership are comparable. |
| Consecutive negative | 32.6 B | 5.1 B | 1.35M → 1.25M | 84% less memory; no material write regression. |
| Widely spaced positive | 37.1 B | 48.8 B | 1.25M → 1.33M | 32% more memory; add/remove and membership throughput are comparable after cardinality tracking was made constant-time. |
| Widely spaced negative | 38.0 B | 48.8 B | 1.27M → 1.33M | 29% more memory; speed is comparable. |

At 50% integer ratio, dense cases use 20.8 B/member versus 35.7–36.2 B/member
for the baseline; sparse cases use 44.5 B/member versus 38.7–39.2 B/member.
At 10% integer ratio, dense cases are near parity (37.9 versus about 40 B),
while sparse cases use 42.7 B/member versus about 40.5 B/member.

### Set algebra and promotion threshold

At 32,768 members and 90% integers, the controlled grouped benchmark measured
the following store latencies (microseconds) at threshold 64:

| Values per group | `SUNIONSTORE` | `SDIFFSTORE` | `SINTERSTORE` | Memory/member, baseline → candidate |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 3,069 → 3,030 | 2,369 → 2,408 | 1,842 → 1,731 | 36.0 → 48.8 B |
| 4 | 2,953 → 1,434 | 2,369 → 1,288 | 1,737 → 1,042 | 35.4 → 15.8 B |
| 8 | 3,120 → 1,331 | 2,438 → 1,082 | 1,801 → 807 | 35.2 → 12.1 B |
| 16 | 3,117 → 1,245 | 2,385 → 1,037 | 1,810 → 790 | 34.8 → 9.4 B |
| 32 | 3,137 → 1,186 | 2,477 → 1,054 | 1,752 → 791 | 34.5 → 8.0 B |
| 64 | 3,055 → 2,140 | 2,423 → 1,150 | 1,801 → 802 | 34.4 → 7.4 B |
| 128 | 3,052 → 2,582 | 2,425 → 1,497 | 1,851 → 1,023 | 34.0 → 7.6 B |

All three operations improve when groups have at least four values. Singleton
groups show no memory or algebra win. For pure dense consecutive values,
candidate union was slower (3,965 µs vs 3,228 baseline), while difference was
2,021 vs 2,361 and intersection 1,801 vs 1,846: difference and intersection
improved slightly, and union regressed.

### Per-group Roaring32 follow-up

The initial per-container Roaring64 version had a write regression after
promotion because each add went through one global 64-bit tree. It was replaced
with one Roaring32 bitmap per promoted upper-48-bit group. The 300-group
single-command benchmark now measures Redis-side average `SADD` at 0.20 µs
before promotion, 1.66 µs for the crossing add, and 0.14 µs for later adds;
the clean `unstable` baseline is 0.22–0.39 µs for these phases. Ordinary
inserts into promoted groups are no longer slower than the baseline, while
crossing the threshold pays a one-time conversion cost. Client round-trip
latencies overlap and are dominated by local request overhead.

The broader 32,768-member / 90%-integer synthetic run showed:

| Pattern | Baseline → Roaring32 memory/member | Add throughput | `SUNIONSTORE` | `SDIFFSTORE` | `SINTERSTORE` |
| --- | ---: | ---: | ---: | ---: | ---: |
| Dense | 31.7 → 5.1 B | 1.24M → 1.12M ops/s | 2,732 → 1,100 µs | 2,113 → 891 µs | 1,484 → 597 µs |
| Sparse | 37.1 → 48.8 B | 1.42M → 1.34M ops/s | 2,536 → 2,983 µs | 2,074 → 2,327 µs | 1,549 → 1,699 µs |
| Negative | 38.0 → 48.8 B | 1.46M → 1.38M ops/s | 2,572 → 3,027 µs | 2,187 → 2,334 µs | 1,562 → 1,673 µs |
| High magnitude | 45.5 → 48.8 B | 1.43M → 1.25M ops/s | 2,504 → 3,080 µs | 2,228 → 2,388 µs | 1,705 → 1,750 µs |

Dense integers retain a large memory and set-algebra win, while sparse or
widely spaced integer groups still consume more memory and can make algebra
slower. Measurements are in `utils/hybrid-roaring32-workload-results.csv` and
`utils/hybrid-roaring32-promotion-latency.csv`.

### Earlier global Roaring64 results (superseded)

A 300-group microbenchmark timed ordinary inserts, the threshold-crossing
insert, and subsequent inserts in promoted groups. At threshold 64, Redis-side
`SADD` averaged 0.19 µs before promotion, 2.84 µs on the crossing insert, and
1.98 µs on subsequent inserts. Baseline hash inserts averaged about 0.3 µs.
Client round-trip percentiles were too noisy to isolate the crossing event, but
server timings show the continuing cost after promotion is larger than the
one-time conversion.

At 32,768 members, threshold-64 bulk add throughput was about 1.37M ops/s at
32 values/group, 0.56M at 64/group, and 0.78M at 128/group; baseline was about
1.28–1.37M. The slowdown repeats once groups promote. A temporary dictionary
of promoted group IDs used about 0.5 B/member more memory and did not improve
write throughput, so it was removed. Removing a redundant membership check
also did not materially change the result. This identified per-integer writes
through the global Roaring64 tree as the bottleneck. Per-group Roaring32
removes that continuing write slowdown, but does not make the representation
beneficial for sparse or widely spaced groups.

Threshold 64 remains the experiment setting because it keeps groups of 32
compact without promotion. Source workload telemetry is not available in this checkout,
so these occupancy profiles are synthetic and cannot tell us how often
production sets have singleton, dense, or clustered integers. Promotion data
is in `utils/hybrid-roaring-promotion-latency-final.csv`; current occupancy
measurements are in `utils/hybrid-roaring-occupancy-64-final.csv`.

## Member expiration cost and recommendation

Redis sets currently expire as keys; there is no per-member expiry. A possible
member-TTL design needs an expiry index keyed by each expiring member, its
deadline, and expiry processing. A dictionary entry, hash-table capacity,
deadline, and copied member key are likely to add roughly 48–80 bytes per
expiring member before fragmentation and background-expiry overhead. That
would erase the dense 90%-integer memory saving at this cardinality and make
the sparse case worse. Member TTL is not implemented here.

**Go/no-go:** the pure-integer path now shows a strong benefit for large dense
sets: memory falls by about 98% at 32,768 members, with set algebra 4–5 times
faster and add/membership throughput close to baseline. Sparse groups pay a
memory penalty, with the worst cost around the 513-member transition. Keep the
feature disabled by default until representative occupancy data confirms how
often the dense case occurs. Next, repeat the A/B matrix on a fixed AWS host and
measure real group occupancy. The macOS set suite and leak checks pass.

## Pre-PR repeated performance sweep

Before opening the draft PR, the mixed-set matrix was rerun three times on the
same local Apple arm64 host. The sweep covered 4,096, 32,768, and 100,000
members; 10%, 50%, and 90% integer ratios; and dense, consecutive-negative,
million-spaced, spaced-negative, and high-magnitude integers. Each round changed
the workload order. Values below are medians of the three runs. This is local
synthetic data, not an AWS or production workload benchmark.

At 90% integers, the dense profile benefits strongly, while sparse membership
and algebra throughput remain close to the hashtable. Sparse memory is the
consistent tradeoff:

| Members / pattern | Memory/member, baseline → candidate | Add rate, baseline → candidate | Membership rate, baseline → candidate | `SUNIONSTORE`, baseline → candidate |
| --- | ---: | ---: | ---: | ---: |
| 32,768 dense | 31.7 → 5.1 B | 1.39M → 1.43M/s | 1.23M → 1.28M/s | 3,075 → 900 µs |
| 32,768 sparse | 37.1 → 48.8 B | 1.37M → 1.33M/s | 1.31M → 1.29M/s | 3,071 → 2,975 µs |
| 100,000 dense | 39.6 → 5.5 B | 1.33M → 1.44M/s | 1.19M → 1.30M/s | 9,984 → 2,783 µs |
| 100,000 sparse | 45.0 → 51.8 B | 1.33M → 1.31M/s | 1.19M → 1.28M/s | 10,147 → 10,330 µs |

Difference and intersection followed the same pattern: large improvements for
dense values and near parity for sparse values. Single-command membership
median/p95 and remove rates were also close between encodings. At lower integer
ratios, memory benefits shrink because strings still use the dictionary.

The full three-round raw matrix is `utils/pr-perf-three-rounds.csv`. The
focused 64-versus-4,097 threshold comparison is in
`utils/bitmap-crossover-threshold-64.csv` and
`utils/bitmap-crossover-threshold-4097.csv`. Delaying the handoff to 4,097 is
not a good choice with the current unsorted temporary vector: at 4,096 values
per group it keeps the group in a linearly searched vector, causing severe
membership and algebra regressions. The 64 threshold hands groups to CRoaring's
adaptive containers early; CRoaring itself keeps small containers as arrays
and switches dense containers to bitmaps.
