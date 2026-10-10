# Native t-digest and completion of the probabilistic stack

All 14 TDIGEST commands live in `src/t_tdigest.c`; the standalone algorithm
is in `src/tdigest.c`, with its upstream MIT notices retained alongside it.
No CMake, third-party test framework or additional library is required.
The data-types team can maintain each native family under `src/t_<type>.c`.

All five families are enabled by default. The compatibility name
`BUILD_BLOOM=yes` currently controls the full family. RedisBloom is removed
from the bundled manifest, so bootstrap, fetching, builds, packaging and
automatic module loading no longer depend on it. Existing ignored local
checkouts are left untouched for compatibility testing.

## Existing keys and persistence

The legacy `TDIS-TYPE` version-0 encoding still loads. Real old-module
restart tests cover RDB and both rewritten-AOF formats, including populated
and empty keys, multiple databases, TTLs and continued writes.

Native version 1 preserves unmerged centroids and integer weights exactly.
Saving no longer compresses the live object or rounds weights through double.
Queries compress a scratch copy, avoiding unreplicated mutations. New saves
cannot be loaded by the old module; retain the pre-upgrade backup for rollback.
See [the cutover procedure](probabilistic-upgrade.md).

Native objects support COPY, expiry, UNLINK, memory accounting, digest and
defragmentation. TYPE returns `tdigest`, the ACL category is `@tdigest`,
and notification class `q` is included in `A`. Merge validates all input
keys and builds a destination before replacing the existing value.

## Tests and review

`make test-tdigest` runs 28 upstream algorithm tests, capacity and adversarial
sort regressions, migrated flow cases and native integration tests.
`make test-probabilistic-upgrade` loads checked-in old-module datasets for all
five types without requiring an external module. Both targets run under
`make test`, event CI and the Ubuntu nightly.

This remains a draft: wider historical-version coverage, allocator/sanitizer
validation and performance review are required. Scratch copies make reads
and transactional updates O(allocated sketch capacity); optimize with care
to preserve replay determinism and atomic failures.
