# Set Roaring benchmark

Build two Redis binaries from the same source revision: one clean baseline and one with the set Roaring changes. The benchmark enables `set-roaring-enabled` only for the candidate.

```sh
python3 utils/benchmark-set-roaring.py \
  --baseline /path/to/baseline/src/redis-server \
  --candidate /path/to/candidate/src/redis-server \
  --out /tmp/set-roaring.csv \
  --sizes 4096,32768,100000 \
  --repetitions 3
```

The script compares dense, negative, sparse, negative-sparse, and high-range integer sets. It records memory per member, add and lookup throughput, incremental add/removal, iteration, and `SUNIONSTORE`, `SDIFFSTORE`, and `SINTERSTORE` latency. The checked-in results CSV is the three-run comparison against clean `unstable` at `6ea763ceb8b417bd35973c828c93afc0d7145e56`, run on Apple Silicon.

For the slice/promotion experiment, use `--candidate-group-bits`, `--candidate-threshold`, and `--candidate-density-percent`. The candidate defaults are 24-bit slices, 64 members, and no density gate. Patterns `stride-N` generate values N apart. The POC 2 decision, summary, and raw CSVs are in [set-roaring-poc2-results.md](set-roaring-poc2-results.md) and [set-roaring-poc2-results/](set-roaring-poc2-results/).
