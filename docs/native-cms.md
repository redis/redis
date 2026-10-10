# Native Count-Min Sketch migration

This is the first follow-up to the native Bloom PR. All six CMS commands now
operate on a native `OBJ_CMS`, with no module API or additional dependency.

## Source and ownership

- `src/t_cms.c`: commands, object lifecycle, persistence, and defragmentation.
- `src/cms.c`, `src/cms.h`: reusable Count-Min Sketch algorithm.
- `src/probabilistic.h`: shared module-compatible wire helpers for native types.
- `src/commands/cms.*.json`: command metadata, including MERGE source-key specs.
- The implementation remains maintained by the data types team in core.

Algorithms and upstream flow cases derive from RedisBloom v8.11.81. Its six
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

The temporary `BUILD_BLOOM=yes` build switch also enables CMS during this stack.
`BUILD_BLOOM=no` disables the native probabilistic family. RedisBloom is no longer bundled.
Do not load RedisBloom alongside native commands with the same names.

## Validation

```sh
make test-cms
make test
```

CI and Daily invoke `test-cms` alongside Bloom. The suite uses Python's standard
library and the existing C toolchain. See `tests/cms/README.md` for migration
details and limitations.
