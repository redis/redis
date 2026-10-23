# Integer-array command benchmark tool

`bitroar-intarray-bench.py` measures `BITRANGEINTARRAY`
in isolated Redis processes over Unix sockets. It requires Python 3.9+, an
optimized Redis build, and no third-party Python packages.

```sh
make -j4
python3 tools/bitroar-intarray-bench.py --json-out /tmp/intarray-native.json
```

The `get-all` workload uses `BITRANGEINTARRAY key` for native full reads and
`R.GETINTARRAY` or `R64.GETINTARRAY` for the module. Range workloads select
ranks on the module and the equivalent `BIT` offset range on the native
backend, from the first to the last selected offset, so both return the same
integers.
Native ranges have no fixed result-count cap. Client output-buffer limits still apply.

To compare with a built redis-roaring module, supply its shared library and
compatible Redis binaries. Use matching compiler and optimization settings,
and record the exact module source revision:

```sh
python3 tools/bitroar-intarray-bench.py \
  --module /path/to/libredis-roaring.so \
  --module-src-dir /path/to/baseline/redis/src \
  --module-revision MODULE_COMMIT \
  --json-out /tmp/intarray-comparison.json
```

Use `--dataset` to select a dataset, `--trials` for repeated samples, and
`--requests`, `--reply-budget-mib`, and `--trial-seconds` to adjust per-trial
budgets. `--clients` and `--pipeline` default to one. At least one full pipeline
batch is sent, even if it exceeds a nominal budget. See `--help` for all options.

Each workload's response and exact command counts are checked before results
are accepted. Both backends use the same data and request count; trial order
alternates. JSON output records raw samples, variation, binary hashes, source
revisions, and build/environment metadata. Keep that file with the results.

Throughput includes consuming the complete reply. redis-benchmark latency
stops at the first socket read, while server execution time comes from INFO
commandstats; neither is full-array end-to-end latency. Run on an idle machine
with identical settings for both backends. Results are diagnostic, not CI gates.

Full methodology, module build instructions, and the historical comparison are
maintained in the [related documentation PR](https://github.com/redis/docs/pull/3715)
in `content/develop/data-types/strings/bitmap-intarray-benchmarks.md`.
