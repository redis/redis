# Native Count-Min Sketch migration

This PR targets `feature/native-probabilistic` independently; see
[the development workflow](probabilistic-development.md).

This independent PR adds all six CMS commands, which now
operate on a native `OBJ_CMS`, with no module API or additional dependency.

## Source and ownership

- `src/probabilistic/t_cms.c`: commands, object lifecycle, persistence, and defragmentation.
- `src/probabilistic/cms.c`, `src/probabilistic/cms.h`: reusable Count-Min Sketch algorithm.
- `src/probabilistic/probabilistic.h`: shared module-compatible wire helpers for native types.
- `src/commands/cms.*.json`: command metadata, including MERGE source-key specs.
- The implementation remains maintained by the data types team in core.

Algorithms and upstream flow cases track RedisBloom master `2fa3a19cbf0a`. Its six
commands, cell sizes (1/2/4/8), signed increments, weighted merges, and RESP3
information maps are retained. Parsing rejects non-finite probabilities and
binary-suffixed keywords. Counters remain 64-bit on 32-bit platforms.

## Compatibility

RDB retains `CMSk-TYPE`: version 0 loads as four-byte cells; version 1 carries
cell size. DUMP/RESTORE works in both directions with external RedisBloom.
Both AOF rewrite formats are supported; command propagation preserves partial
successes. Native TYPE/SCAN report `cms`. COPY, expiration, lazy freeing,
memory accounting, digest, ACL category `@cms`, and notifications are integrated.
Notification class `M` is included in `A`; existing classes are unchanged.
Native memory usage intentionally excludes module-wrapper overhead.

`BUILD_BLOOM=yes` enables the migrated families present in this checkout.
`BUILD_BLOOM=no` disables them. RedisBloom retirement is a separate final PR;
do not load the module alongside native commands with the same names.

## Validation

```sh
make test-cms
make test
```

CI and Daily invoke `make test-probabilistic`, which includes this suite. The suite uses Python's standard
library and the existing C toolchain. See `tests/cms/README.md` for migration
details and limitations.
