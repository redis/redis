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
queries reuse that view until the next mutation. A separate sorted index of the
source centroids survives appends, so alternating writes and queries no longer
sort the unchanged input again. Up to eight new centroids use binary insertion;
larger appends sort only the new tail and merge it with the retained prefix.
The index has bounded O(n + added*log(added)) work for a large append, and ordered
or identical input skips sorting. Canonical write compression, RESET and MERGE
discard the index because the source centroid positions have changed.

Queries resolve equal means by their original centroid position. This makes
cached and cold queries identical regardless of read history; tie ordering can
differ from the previous unstable query sort. The writer's compression policy,
persisted arrays and statistical algorithm remain unchanged.

The retained view contains only compressed centroids; no second full-capacity
input buffer is retained. The cache adds one pointer per digest, a lazy cache
header, up to four bytes per reserved index entry, and a histogram header plus
16 bytes per compressed centroid (before allocator rounding). Index capacity
grows geometrically and is capped by the source capacity. The index survives
appends while the compressed answer is discarded. A large unsorted append may
also use a temporary four-byte-per-live-node merge buffer.
Both MEMORY USAGE and TDIGEST.INFO include the cache. COPY and persistence omit
it; defragmentation, lazy freeing and memory tracking account for it.

Alternating writes and queries still scan and compress the live source nodes on
each read, preserving saved state rather than compacting it as a read side
effect. This remains a draft requiring broader historical-version and
platform/performance coverage.
