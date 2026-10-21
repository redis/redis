# Set Roaring POC 2: slice width and promotion

## Decision

Use 24-bit slices, keep a sorted sparse array in each slice, and promote a slice to the merged Roaring bitmap at 64 members. Keep the density gate off by default. This combines online grouping with Roaring where it is useful: a slice holding only a few far-apart values stays an array; a slice with many values gets Roaring's compact containers.

The POC keeps the existing 512-member `intset` limit. Strings remain in a hashtable, and canonical signed 64-bit integers use the integer path. `"01"` remains a string, distinct from integer `1`.

## Why 24 bits

Values are first mapped to order-preserving unsigned 64-bit values, then grouped by their upper bits. A 24-bit slice covers 16,777,216 adjacent encoded values. Each sparse slice stores its low values in sorted order, so membership uses binary search. When its count reaches 64, only that slice is inserted into the shared Roaring bitmap. The Roaring bitmap still decides its own internal container representation.

This naturally handles different live streams without knowing the future. With values about 1,000,000 apart, a 24-bit slice gets about 16 values and stays sparse. With values 1,000 apart, the slice collects thousands of values and promotes. Tests with 20-, 24-, 28-, and 32-bit slices show why the slice width matters:

| Slice bits | 100k values, about 1M apart | Result |
| ---: | ---: | --- |
| 20 | 64.0 B/member | Too many lightly populated slice records |
| 24 | 10.9 B/member | Sparse values stay compact; regular groups promote |
| 28 | 93.5 B/member | Far-apart values promote into many tiny Roaring containers |
| 32 | 94.3 B/member | Same over-promotion, with larger sparse buckets |

The density gate was tested at 1%, 3%, 6%, and 25%. It did not improve on the 64-member rule for these workloads. At 25%, it delayed useful promotion: for 100k values spaced by 10, memory rose from 1.67 to 5.49 B/member; for spacing 1,000 it rose from 3.92 to 8.28 B/member. A 1% gate allowed spacing-100 values to promote, but still did not help spacing-1,000 values. The gate remains configurable for future workloads, but its default is off.

## Results

The candidate was compared with a clean build of `unstable` at `6ea763ceb8b417bd35973c828c93afc0d7145e56`. Both builds used the same local Apple Silicon machine and Apple clang 21; Redis was built with the repository's normal `make -C src` settings. Each benchmark configuration ran three times. The figures below are medians; small throughput differences are within local-run noise.

At 100k members, with the 24-bit slices and 64-member promotion threshold:

| Input pattern | Clean unstable | Candidate | Memory change |
| --- | ---: | ---: | ---: |
| Consecutive positive integers | 38.6 B/member | 0.3 B/member | −99% |
| Values about 1M apart | 44.6 B/member | 10.9 B/member | −76% |
| Negative values about 1M apart | 45.6 B/member | 10.9 B/member | −76% |
| High-magnitude negative values about 1M apart | 53.7 B/member | 10.9 B/member | −80% |
| Values spaced by 10 | 39.6 B/member | 1.7 B/member | −96% |
| Values spaced by 100 | 40.6 B/member | 2.6 B/member | −94% |
| Values spaced by 1,000 | 41.6 B/member | 3.9 B/member | −91% |

At the `intset` boundary, both builds use about 2–8 B/member at 512 values. At 513, the candidate uses about 20–28 B/member versus about 37–54 B/member in the baseline hashtable, depending on the value distribution.

For 100k-member sets, add, remove, membership, and full iteration were broadly flat. The sparse/negative/high-range cases showed roughly 0–6% add changes, −3% to +8% membership changes, and near-zero iteration changes. Set algebra was mixed: `SUNIONSTORE` latency fell about 53–55%, `SDIFFSTORE` fell about 10–27%, while `SINTERSTORE` latency rose about 4–14% for sparse/high-range inputs. Dense `SINTERSTORE` was effectively flat. These are single-machine synthetic workloads, so the larger memory and algebra signals are more useful than small throughput deltas.

## Correctness and remaining work

The set unit suite passes, including the new POC regression test. Targeted live-server checks also passed for slice widths 16, 24, 28, and 32 with sparse and promoted integers, negative and high-magnitude values, strings, reads, removals, random operations, set algebra, `COPY`, and key TTL. A 100k-member sparse-slice stress run passed after widening sparse bucket counters from 16 bits to `size_t`.

Member-level expiry is still out of scope; existing key-level expiry works across the encoding. A per-member expiry index would need one deadline record for each expiring member. A rough 64-bit hash-index estimate is 32–48 additional bytes per expiring member for the index entry, bucket share, and deadline storage, before any copied string key or allocator rounding. A packed integer index could be closer to 16 bytes per member for an 8-byte value plus an 8-byte deadline, but would need its own lookup and expiry-processing design. These are structural estimates, not benchmark measurements; the overhead could erase Roaring's savings when most members expire individually.

Changing `set-roaring-group-bits` affects sets created after the change. Each existing hybrid set keeps the slice width it was created with.

The first POC checkpoint is commit `add3021260a29942445904f356baef5d612be056` on branch `set-roaring-bitmap-reuse`. POC 2 is based on its PR update at `91a42a8c980edf75196f1371c70276be8309d8c1`; it has not replaced that checkpoint.

Before a production decision, rerun the best 24-bit configuration on the primary x86 and ARM bare-metal benchmark hosts with at least three repetitions, especially `SINTERSTORE` and the intset-to-hybrid transition. The local runs do not justify a claim about fleet-wide latency.

## Raw data

All per-run CSVs are in [`set-roaring-poc2-results/`](set-roaring-poc2-results/). `poc2-group-*-density-0-final.csv` contains the final slice-width comparison; `poc2-group-24-transition.csv` contains the 512/513 check; `poc2-strides-density-*.csv` contains the promotion-gate sweep.
