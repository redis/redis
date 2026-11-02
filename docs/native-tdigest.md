# Native t-digest

This PR targets `feature/native-probabilistic` independently; see
[the development workflow](probabilistic-development.md).

All 14 TDIGEST commands live in `src/probabilistic/t_tdigest.c`; the standalone algorithm
is in `src/probabilistic/tdigest.c`, with its upstream MIT notices retained alongside it.
No CMake, third-party test framework or additional library is required.
The data-types team can maintain each native family under `src/probabilistic/t_<type>.c`.

This PR adds only t-digest. The compatibility name `BUILD_BLOOM=yes`
enables migrated families present in the checkout. RedisBloom retirement
and cross-family fixture validation are separate, last-to-merge work.

## Existing keys and persistence

The legacy `TDIS-TYPE` version-0 encoding still loads. Real old-module
restart tests cover RDB and both rewritten-AOF formats, including populated
and empty keys, multiple databases, TTLs and continued writes.

Native version 1 preserves unmerged centroids and integer weights exactly.
Saving no longer compresses the live object or rounds weights through double.
Queries compress a scratch copy, avoiding unreplicated mutations. New saves
cannot be loaded by the old module; backward readability is not required.
See [the cutover procedure](probabilistic-upgrade.md).

Native objects support COPY, expiry, UNLINK, memory accounting, digest and
defragmentation. TYPE returns `tdigest`, the ACL category is `@tdigest`,
and notification class `q` is included in `A`. Merge validates all input
keys and builds a destination before replacing the existing value.

## Tests and review

`make test-tdigest` runs 28 upstream algorithm tests plus an exact-weight regression, capacity and adversarial
sort regressions, migrated flow cases and native integration tests.
This suite runs under `make test`, event CI and the Ubuntu nightly through
`make test-probabilistic`. Cross-family fixtures are in the separate retirement PR.

This remains a draft: wider historical-version coverage, allocator/sanitizer
validation and performance review are required. Scratch copies make reads
and transactional updates O(allocated sketch capacity); optimize with care
to preserve replay determinism and atomic failures.
