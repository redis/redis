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
Queries use a separate compressed view, avoiding unreplicated mutations. New saves
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

## Append and query costs

ADD validates the entire batch before appending in place when it fits without
compression. Batches that may compress retain the transactional copy, preserving
atomic failure. Copies reserve the original capacity but copy only live nodes.

The first query after a write compresses a copy of the live nodes. Subsequent
queries reuse that view until the next mutation. The retained arrays contain
only compressed centroids; no second full-capacity input buffer is retained.
The cache adds one pointer per digest, plus a header and 16 bytes per retained
centroid on keys queried since their last write (before allocator rounding).
Both MEMORY USAGE and TDIGEST.INFO include the cache. COPY and persistence omit
it; defragmentation, lazy freeing and memory tracking account for it.

Alternating writes and queries still pay the first-query compression cost on
every read. This remains a draft requiring broader historical-version and
platform/performance coverage.
