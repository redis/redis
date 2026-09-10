# Compact B+ tree sorted sets

Updated: 2026-08-05

This is the canonical design and practical report for the implementation in
`zset-mem-saving-alt-final`. It describes the current working tree, including
the external storage used by members of 320 bytes or more. Earlier sorted-set
memory reports remain useful as experiment logs, but their macro-node skiplist
and oversized-member fallback descriptions are not the current design.

## Executive summary

This branch keeps Redis's listpack encoding for small sorted sets and replaces
the skiplist plus general-purpose dictionary used by large sorted sets with:

* one B+ tree ordered by `(score, member)`; and
* one compact, sorted-set-specific hash table that maps a member to the score
  leaf containing it.

The score tree owns every member. The hash table stores only an 8 bit hash tag
and a 16 or 32 bit leaf number. There is no second member string and no
allocation per hash entry. Scores, member offsets, hash tags, and short member
bytes are packed together in score leaves. A member of 320 bytes or more has
one separate allocation; its leaf record stores the pointer, length, and hash.
This prevents one large member from forcing the complete sorted set back to the
skiplist or making ordinary leaf edits copy a large string.

In the primary benchmark, 200,000 random 8 to 40 byte alphanumeric members and
random scores use 8,710,404 bytes according to `MEMORY USAGE`, versus
22,099,276 bytes on unstable. This is a 60.59% reduction. Measured RSS growth
falls from 19,988,480 to 10,878,976 bytes, a 45.57% reduction.

After 200,000 random score changes, the object remains 54.23% smaller and its
RSS growth remains 34.61% lower than unstable. The representation therefore
retains most of its saving after edits instead of depending on a freshly built
shape.

That speed comparison used identically compiled `-O3 -flto`, libc,
non-sanitized binaries. Building the random 200K set was 4.28% slower end to end
and 8.40% slower inside Redis. Random successful `ZSCORE` calls were tied,
within 0.16% inside Redis. Missing-member lookups were 50.40% faster inside
Redis, random score changes were 21.43% faster, and `ZRANK` was 67.83% faster.
Deleting 180,000 members in one rank, score, or lex range was 93% to 95% faster
inside Redis.

Successful lookup has an important locality-dependent result. Looking up the
members in their original insertion order is 23.12% slower inside Redis;
looking them up in score order is 10.19% faster. Unstable allocates skiplist
nodes in insertion order, while this implementation packs strings in score
order. Random lookup, which does not favor either layout, is effectively even.

Small listpack sorted sets are unchanged. Every normal large sorted set now
uses the B+ tree, regardless of member length. The skiplist remains in the
source for explicit internal conversions and as temporary storage in the
union, intersection, and difference implementation. That temporary use is not
fundamental and is one of the main remaining design problems.

The RDB type, AOF form, replication stream, command replies, and module API are
unchanged. Old and new Redis binaries can exchange data in both directions.

## Encoding selection

The default small encoding is still the listpack. A sorted set remains a
listpack while its count, largest member, and total listpack size satisfy the
existing configured limits. By default the important limits are 128 members
and 64 bytes for one member.

Once a set leaves the listpack, its normal representation is the B+ tree. A
large member changes only how that member is stored; it does not change the
encoding of the object. This distinction matters because representation
selection should follow what the complete set needs. A local storage detail
must not require a second complete implementation.

The skiplist is therefore no longer a required final encoding. It is still
reachable because some bulk commands construct a temporary skiplist and then
convert stored results to a listpack or B+ tree. The conversion functions also
retain explicit skiplist support. Removing these uses requires a direct bulk
B+ tree builder, described under remaining design work.

## Representation

### Score tree

The B+ tree is ordered by score, with the member bytes breaking equal-score
ties. Leaves are linked in both directions, so a range performs one tree seek
and then walks consecutive leaves.

A score leaf is one allocation of at most 4095 bytes and at most 96 members:

```
header | packed scores | u16 record offsets | hash tags | free | member records
```

Member records grow backward from the end. The arrays grow forward. The free
space between them allows common inserts and score changes to stay in the same
allocation.

An inline member record is a compact length followed by the member bytes. An
external member record contains a marker, a 32 bit hash, a 64 bit length, and a
pointer. The separately allocated member bytes have no SDS header. The score
leaf is their only owner.

The 320-byte boundary was measured rather than chosen from the old 1024-byte
limit. Sweeps from 64 through 4096 bytes, followed by focused tests from 288
through 320 bytes on macOS/libc and Linux/jemalloc, found that external storage
becomes clearly preferable around this point. At 320 bytes it reduces object
memory and makes builds, score changes, and leaf deletion substantially
faster. At 319 bytes the inline representation is retained.

When a leaf is rebuilt or a member changes score, ownership of an external
allocation moves to the new record. The member bytes are not copied. Deletion
frees the allocation, COPY duplicates it, active defragmentation may move it,
and fork-child memory dismissal visits it.

Scores are stored exactly. Each IEEE double is converted to an integer with the
same order. A leaf stores one base plus bit-packed differences. Shared low zero
bits are recorded once and omitted from every difference. This is especially
effective for nearby integer scores, but does not lose precision for arbitrary
doubles.

Each inner page has up to 32 children. Beside every child pointer it stores the
maximum score and the number of members below that child. These counts provide
rank operations without a second rank structure.

### Member index

Member lookup uses open addressing with eight slots per bucket. A slot stores:

```
one-byte hash tag | score-leaf number
```

Most sets use 16 bit leaf numbers. The table changes incrementally to 32 bit
numbers before that range can fill. Neither form stores a score, member pointer,
complete hash, or separately allocated entry.

A lookup computes the complete hash, checks the one-byte tags in candidate
buckets, then follows a matching leaf number and compares the real member
bytes. Tags can only reject candidates. A collision can cause extra work but
cannot return the wrong member.

Each bucket also has a 64 bit summary of tags whose hashes started at that
bucket. Most absent members are rejected by this summary before an open-address
table walk. The summary can report "perhaps" but never "present". It is kept
beside the bucket, so the first tag check and the summary normally arrive in
the same cache line.

Deleted table slots retain a tombstone so a probe can continue past them. The
table grows, shrinks, and clears tombstones incrementally. Migration moves one
complete score leaf at a time. Inline records provide the complete member
hash; external records cache it so migration does not read a long allocation.

At the start of a resize the complete destination table is allocated. Normal
commands then copy one or more leaves. Old entries are left in place, but a
number in each copied leaf tells lookup to ignore its old entries and use the
new table. New writes go to the table that owns that leaf. Once every live leaf
has been copied, the old table is freed. This bounds ordinary command work, but
temporarily retains both bucket arrays.

### Stable leaf numbers

Hash entries refer to score leaves by number rather than address. A small
`score_leaf_by_id` array translates that number to the current allocation.
Compaction and active defragmentation can therefore move a leaf without
rewriting every member entry that refers to it.

A replacement retains its number. A split retains the number on the left and
allocates one for the right. A merge retains the left number. Released numbers
form a free list inside the translation array itself.

### Main invariants

The implementation depends on a small number of ownership and lifetime rules:

* The score tree is the only owner of member data. The member index never owns
  or frees a member.
* An external pointer belongs to exactly one leaf record. A leaf rebuild clears
  the old record when the new record adopts that pointer.
* A live member-index entry names a live leaf. During an incremental resize,
  the leaf's resize number says whether its entries are still in the old table
  or already in the new one.
* Subtree counts in parent pages equal the number of members below each child.
  Rank operations rely on these counts being repaired after every structural
  change.
* Iterators and returned member pointers belong to score leaves or their owned
  external allocations. They remain valid only until the next modification of
  that sorted set.

### Main operation paths

`ZSCORE` hashes the member, finds a leaf number in the member table, and checks
matching tags in that leaf. `ZRANK` starts with the same lookup, then adds the
stored child counts while walking from the leaf to the root.

An insertion searches both indexes once. It rebuilds or splits one leaf and
repairs parent counts. A score change removes the old ordered record and
inserts it at its new position; an external member transfers its existing
allocation rather than copying its bytes. Deletion can join neighboring leaves
when the combined count and byte size fit.

Rank and score ranges search the tree once and then follow linked leaves. Large
range deletion removes runs of records and rebuilds the member index from the
survivors instead of maintaining it once per deleted member. `ZSCAN` walks the
member table because its result is intentionally unordered.

## Why memory falls

The unstable large-zset representation has one skiplist node and one dictionary
entry per member. It pays for allocator rounding, several pointers, skiplist
levels and spans, dictionary metadata, and buckets in addition to the string
and score.

This implementation removes most of that fixed cost:

* Up to 96 ordered members share one leaf header and one tree position.
* Member strings exist once. Short strings are inline; long strings have one
  allocation owned by a compact leaf record.
* Hash entries are fields inside bucket arrays, not individual allocations.
* Hash entries normally need three bytes: one tag and one 16 bit leaf number.
* Scores use the number of bits needed by one leaf's score range.
* Rank counts exist once per tree child, not at probabilistic skiplist levels.
* Linked leaves give range operations locality without one pointer-bearing
  node per member.

There is no arena or pointer-index allocation scheme in this design.

## Benchmark evidence

There are two benchmark snapshots below. The first is the broad, high-sample
comparison against unstable made before external member storage was added. Its
8-to-40-byte members all use the unchanged inline path, so it remains the best
measurement of the main use case. The second was run on the current working
tree and includes members large enough to exercise both storage forms.

Results from different snapshots are not combined into one claimed total.
Allocator state, binary revision, and workload shape all matter at the scale of
the smaller timing differences.

### Primary short-member method

The primary comparison used:

* unstable commit `f6ffa0003`;
* the packed B+ tree on `zset-mem-saving-alt-final`, immediately before the
  external-member addition;
* `-O3 -flto`, libc, no ASAN or UBSAN for both binaries;
* an Apple M3 Max running macOS;
* a fresh Redis process and a 100 ms idle period for every sample; and
* randomized implementation order.

The primary data set contains 200,000 unique alphanumeric members with lengths
uniformly distributed from 8 through 40 bytes and independent random scores.
Build figures are medians of 31 runs. Command figures are medians of 15 runs.
Redis command CPU time comes from `INFO commandstats`; wall time also includes
protocol and client work.

### Primary memory results

All values are bytes.

| State | Unstable object | B+ object | Change | Unstable RSS | B+ RSS | Change |
|---|---:|---:|---:|---:|---:|---:|
| Fresh random 200K | 22,099,276 | 8,710,404 | -60.59% | 19,988,480 | 10,878,976 | -45.57% |
| After 200K random score changes | 21,052,796 | 9,636,100 | -54.23% | 20,119,552 | 13,156,352 | -34.61% |

Read-only work can finish an incremental resize in unstable and release its old
table. After 500,000 successful lookups, unstable accounted 21,051,948 bytes;
the B+ object remained 8,710,404 bytes, still 58.62% smaller. Both the pre- and
post-resize numbers are reported so an unfinished resize is not mistaken for a
permanent saving.

The saving is largest for short members, where fixed data-structure overhead
matters most. For members hundreds of bytes long, payload bytes dominate and
the percentage falls. The current code keeps those members in the B+ tree but
stores each one separately once it reaches 320 bytes.

### Primary speed results

| Workload | Unstable wall | B+ wall | Wall change | Unstable Redis | B+ Redis | Redis change |
|---|---:|---:|---:|---:|---:|---:|
| Build random 200K | 231.51 ms | 241.41 ms | +4.28% | 144.09 ms | 156.20 ms | +8.40% |
| `ZSCORE`, 500K random hits | 299.24 ms | 299.64 ms | +0.13% | 135.03 ms | 135.25 ms | +0.16% |
| `ZSCORE`, 500K misses | 223.37 ms | 196.41 ms | -12.07% | 58.50 ms | 29.02 ms | -50.40% |
| `ZSCORE`, 500K hits in score order | 278.21 ms | 267.65 ms | -3.80% | 116.93 ms | 105.01 ms | -10.19% |
| `ZSCORE`, 500K hits in insertion order | 273.43 ms | 304.64 ms | +11.42% | 111.84 ms | 137.70 ms | +23.12% |
| `ZRANK`, 200K hits | 205.56 ms | 120.42 ms | -41.42% | 136.73 ms | 43.99 ms | -67.83% |
| 200K random score changes | 321.52 ms | 269.72 ms | -16.11% | 236.66 ms | 185.95 ms | -21.43% |
| Delete 180K by rank | 30.88 ms | 6.66 ms | -78.43% | 25.37 ms | 1.69 ms | -93.32% |
| Delete 180K by score | 22.79 ms | 5.55 ms | -75.67% | 17.82 ms | 0.99 ms | -94.47% |
| Delete 180K by lex | 23.90 ms | 5.96 ms | -75.06% | 18.86 ms | 0.96 ms | -94.92% |

A successful member lookup follows the compact hash result into a score leaf
and compares only members with the same one-byte tag. Random probes show that
this extra step has no practical cost. Ordered probes expose the placement of
the member bytes: unstable is favored by insertion order and the B+ tree is
favored by score order. Preserving both orders would require another per-member
link or copy and would give back part of the memory saving.

Rank and large range deletion benefit from packed leaves and subtree counts.
Large deletions use a separate path that keeps the survivors instead of paying
the fixed tree and index maintenance cost once for every removed member. COPY
and range iteration were also faster in the earlier broad workload matrix; the
table above contains the fresh exact-binary measurements of the most important
paths.

### Mixed-size current-working-tree result

This comparison was run after external members were implemented. Both binaries
were optimized, used libc, contained no sanitizer, and ran in randomized order
on the same Apple M3 Max. Each figure is the median of three fresh processes.

The set had 30,000 members with this repeating length distribution:

```
24:70%, 96:15%, 192:5%, 320:4%, 1024:4%, 4096:2%
```

The average payload is 176.48 bytes. The 4 KB members make only 2% of the set,
but the previous design switched the whole object to the skiplist because of
them. The current design keeps one B+ tree and stores just those members
externally.

| Measurement | Unstable | Current B+ tree | Change |
|---|---:|---:|---:|
| Fresh object memory | 8,629,804 B | 6,001,668 B | -30.45% |
| Fresh RSS growth | 8,978,432 B | 6,832,128 B | -23.91% |
| Build wall time | 71.79 ms | 77.71 ms | +8.26% |
| Build Redis time per member | 0.596 us | 0.735 us | +23.24% |
| `ZSCORE` wall time, 60K hits | 123.87 ms | 131.56 ms | +6.21% |
| `ZSCORE` Redis time per hit | 0.204 us | 0.302 us | +47.77% |
| `ZRANK` wall time, 60K hits | 127.65 ms | 117.77 ms | -7.75% |
| `ZRANK` Redis time per hit | 0.502 us | 0.257 us | -48.88% |
| Change 15K scores, wall time | 35.07 ms | 35.14 ms | +0.19% |
| Change one score, Redis time | 0.548 us | 0.552 us | +0.84% |
| Delete 15K by rank, Redis time | 1.76 ms | 0.81 ms | -54.03% |
| Object memory after score changes | 8,629,804 B | 6,751,620 B | -21.76% |
| Object memory after deleting half | 4,444,252 B | 3,374,084 B | -24.08% |

This workload exposes the remaining point-lookup tradeoff. Unstable's
dictionary points directly to one skiplist node. The compact member index
points to a B+ tree leaf and then checks members in that leaf. The extra check
is cheap for the short-member benchmark, but visible when leaves contain this
mixture of inline and separately allocated members. Rank, updates, and large
deletion remain even or faster.

### Why 320 bytes

The threshold experiment compared the current external record with the old
inline record while keeping the rest of the B+ tree identical. At exactly 320
bytes and 30,000 members, external storage changed the following medians:

| Measurement | Inline | External | Change |
|---|---:|---:|---:|
| Object memory | 11,851,524 B | 10,750,724 B | -9.29% |
| Build Redis time per member | 1.049 us | 0.693 us | -33.88% |
| `ZSCORE` Redis time per hit | 0.365 us | 0.283 us | -22.29% |
| Change one score, Redis time | 0.650 us | 0.524 us | -19.30% |
| Delete half by rank | 2.18 ms | 1.09 ms | -50.02% |

The object-memory crossover appeared around 288 to 296 bytes. The rounded
320-byte boundary was chosen conservatively: by that point the memory and
overall write-speed wins were clear across focused macOS/libc and
Linux/jemalloc tests, without creating separate allocations for borderline
medium-sized members. Values below 320 remain inline by policy, not because
319 is a sharp physical boundary. The threshold is a compiled implementation
choice, not a user configuration whose compatibility would need to be
maintained.

For 30,000 fixed 4 KB members, the same experiment compared the former
skiplist fallback with the external-member B+ tree:

| Measurement | Former skiplist | External B+ tree | Change |
|---|---:|---:|---:|
| Object memory | 154,582,924 B | 124,030,724 B | -19.76% |
| RSS growth | 159,006,720 B | 126,697,472 B | -20.32% |
| Build wall time | 226.49 ms | 184.53 ms | -18.53% |
| Build Redis time | 117.67 ms | 70.59 ms | -40.01% |
| `ZSCORE` Redis time, 60K hits | 107.94 ms | 104.85 ms | -2.86% |
| `ZRANK` Redis time, 60K hits | 154.00 ms | 118.19 ms | -23.25% |
| Change 15K scores, Redis time | 39.59 ms | 30.03 ms | -24.13% |
| Delete half by rank, Redis time | 18.91 ms | 2.15 ms | -88.63% |

Large payload bytes dominate both representations, so a 19.76% object saving
is still substantial. More importantly, preserving one ordered structure
avoids the old fallback without charging the common short-member case.

## Review-driven improvements

An interactive Fable review and a separate manual audit found five correctness
or compatibility problems, all fixed in this pass:

1. B+ `ZSCAN` formatted its 64 bit cursor through a signed integer. A process
   that reached revision bit 31 could return a negative cursor that the next
   call rejected. Cursor formatting is now unsigned.
2. B+ `ZSCAN` returned scores as RESP3 doubles, while existing Redis returns
   bulk strings for this command. It now uses the same exact text and reply
   type as the skiplist path.
3. A module range iterator could retain a rank made invalid by a module-side
   deletion and abort Redis on an assertion. It now ends the iteration.
4. Listpack-to-B+ conversion trusted member uniqueness without checking it.
   It now rejects a duplicate in the same manner as the unstable conversion.
5. `ZSCAN` could skip a member that remained in the set for a complete scan if
   inserts or score changes moved members between leaves. Several members in
   one leaf can have the same 8 bit tag, and the old code made one table slot
   stand for all of them. An edit could move that chosen slot behind the
   cursor. The scanner now returns a colliding member from every table slot
   that its normal lookup can reach. This may return a duplicate, which SCAN
   permits, but cannot hide a member by changing which slot stands for it.
   Table copying during a resize keeps the cursor valid; installing the new
   table changes its scan number and restarts the cursor once.

The same pass made three measured or mechanical improvements:

* Score bounds inside a leaf now use the existing binary search. Forward and
  reverse score seeks use about 2.8% to 3.0% less Redis CPU, with no memory
  change and six fewer lines.
* The per-home rejection summary moved from a parallel array into its bucket.
  Total bytes are identical. Fifteen-run comparisons against the previous
  layout reduced Redis CPU by 2.35% for hits, 6.90% for misses, 1.75% for
  near-full hits, 3.38% for near-full misses, and 2.09% for local score edits.
* Dead masks for four unused offset bits and an impossible allocation branch
  were removed. This shortens hot paths and lets sanitizers expose a bad offset
  instead of silently masking it.

The final review also made resize wraparound explicit, stopped a module scan if
its callback changes the object encoding, removed dead update state, and
simplified insertion into a deleted hash slot. Those changes do not alter the
representation or its memory use. The later external-member change removed the
long-member conversion that previously made the scan guard and RDB fallback
path especially important.

The corrected `ZSCAN` path was measured on a settled 200,000-member table over
11 paired runs. Median wall time fell from 426.902 to 418.817 milliseconds and
Redis CPU time fell from 100.092 to 93.523 milliseconds. It returned between
zero and eight permitted duplicate entries per run and used exactly the same
8,396,804 object bytes. The correctness change therefore did not trade memory
or scan speed for safety.

Two tempting memory experiments were rejected before correctness work:

* Removing the 64 bit home summary saved 3.01% object memory but increased
  missing-lookup Redis CPU by 67.8%.
* Keeping only a 32 bit summary saved 1.50% object memory but increased hit CPU
  by 5.3% and miss CPU by 11.1%.

These fail the requirement that memory savings must not damage speed.

## Persistence and compatibility

No new RDB type was introduced. B+ zsets are written as the existing
`RDB_TYPE_ZSET_2` stream and old Redis loads them as skiplists. Existing RDBs
load through the normal path. Small sets become listpacks and large sets become
B+ trees, regardless of member length. DUMP/RESTORE, AOF rewrite, and
replication use existing external formats.

Cross-version QA covered RDB, DUMP/RESTORE, rewritten AOF, and replication in
both old-to-new and new-to-old directions. The branch can therefore be swapped
with unstable without an on-disk migration.

The loader creates the representation selected by the stored element count and
adds members through the common sorted-set path. Small sets remain listpacks,
while large sets enter the B+ tree immediately instead of first building a
skiplist and dictionary. The load uses NX behavior to report a duplicate member
as a corrupt RDB. RDB already provides the exact element count and writes large
zsets in score order, but the current B+ tree API does not yet exploit that
information to reserve or build the complete tree in one pass.

The observable internal encoding does change: `OBJECT ENCODING` reports
`btree` instead of `skiplist` for normal large sorted sets. `MEMORY USAGE`, scan
order, and opaque `ZSCAN` cursor values also change. Applications must already
treat scan order and cursor contents as unspecified. Command results, score
formatting, persistence, replication, and module API behavior remain the
compatibility boundary.

## Active defragmentation and fork memory

Active defragmentation is implemented for the B+ encoding. It moves the set,
hash allocations, leaf-number array, score leaves, inner pages, and external
members while repairing every affected pointer. Its cursor records a leaf
number and a member position. One incremental call moves at most one leaf and
one external member, so a leaf containing many large members does not turn one
defragmentation step into many large copies. The cursor lives in the
defragmenter, not in each sorted set, so inactive sets pay no permanent bytes
for it.

In a Linux jemalloc fork child, Redis dismisses the member-index arrays,
leaf-number array, score leaves, and external member allocations after
serializing the key. This reduces copy-on-write RSS during BGSAVE and AOF
rewrite. It does not run in the parent command path and is a no-op on
unsupported allocator/platform builds.

## Code size and complexity

Compared with unstable, the current working tree adds 4,467 net production
lines:

* `zset_btree.c`: 3,651 lines;
* `zset_btree.h`: 116 lines; and
* integration in zset commands, modules, sort, persistence, scan, memory
  accounting, and defragmentation: 700 net lines.

Tests add another 664 net lines.

This is materially more code than the skiplist representation. The complexity
is concentrated in one private implementation rather than presented as a
general-purpose tree library. The main sources of complexity are exact packed
scores, leaf replacement and splitting, incremental member-index resizing,
range deletion, active defragmentation, and support for three zset encodings.

The review removed dead offset state, reused one score-bound helper, kept hash
filters in the bucket allocation, and normalized conversion insertion into one
path. A generic B+ tree or dictionary would need more per-entry state. There is,
however, one credible large simplification still available: build bulk results
directly in the B+ tree and then remove the skiplist as a normal construction
tool.

## Correctness and QA

### Packed B+ tree QA

The packed B+ tree before the external-member addition passed:

* the complete `unit/type/zset` suite;
* standalone and cluster `unit/scan`, including raw RESP3 score frames;
* `unit/moduleapi/zset`, including invalidating a saved range rank;
* `unit/keyspace`, including COPY during member-index growth;
* `unit/sort` and `unit/geo`;
* `unit/memefficiency` and the associated defrag test;
* 787,614 differential commands against unstable over three fresh seeds in the
  optimized build, including random writes, rank and score deletion,
  equal-score runs spanning leaves, lex ranges, set operations, binary
  members, infinities, and the old 1024/1025 byte boundary;
* forced migration to 32 bit leaf numbers with 117,000 1024-byte members and
  142,034 compared commands; and
* RDB, DUMP/RESTORE, AOF rewrite, and replication in both directions.

The exact final source was then rebuilt in isolated worktrees, so none of the
sanitizer flags could enter the optimized benchmark binaries:

* AddressSanitizer with `DEBUG_ASSERTIONS`: `unit/type/zset`, standalone and
  cluster `unit/scan`, `unit/moduleapi/zset`, `unit/moduleapi/scan`,
  `integration/rdb`, `integration/convert-ziplist-zset-on-load`,
  `unit/keyspace`, `unit/sort`, `unit/geo`, and `unit/memefficiency`, including
  its defrag phase, plus 791,905 differential comparisons over three new seeds;
* UndefinedBehaviorSanitizer with `DEBUG_ASSERTIONS`: the complete zset and
  scan suites, RDB integration, and 790,704 differential comparisons over
  three new seeds; and
* `DEBUG_DEFRAG_FORCE` with `DEBUG_ASSERTIONS`: an active-defrag stress test
  that moved 40,480,229 allocations while changing scores, deleting ranges,
  copying keys, exercising the former skiplist fallback, comparing full
  contents and digests, and saving an RDB;
* a Linux arm64 GCC 13 build with jemalloc and `DEBUG_ASSERTIONS`, followed by
  `integration/dismiss-mem` with explicit listpack and B+ zsets. Both RDB and
  plain-command AOF rewrite paths completed and reloaded with matching
  database digests.

An independent read-only review covered every implementation hunk before the
final mechanical cleanup. Later adversarial scan testing found the same-tag
cursor bug described above: the old code missed 4 to 11 stable members in each
of ten fresh hash-seed runs. The corrected code missed none, and a broader
30-seed run mixed insertion, deletion, score changes, and table copying without
missing a stable member. Fable reviewed the correction and both permanent
regression tests and found no blocking defect.

### External-member QA

The current working tree adds tests at 319, 320, and 321 bytes and with 4 KB
members. They cover insertion, direct lookup, rank, score changes, leaf splits
and joins, individual and range deletion, pop operations, COPY independence,
set operations, RDB reload, AOF rewrite, active defragmentation, and fork-child
memory dismissal.

The exact source revision used for the final commit was compiled with `-O3`,
LTO, libc, and no sanitizer flags, then passed all 150 units and every solo
test in `./runtest --clients 8`. This includes `unit/type/zset`, standalone and
cluster `unit/scan`, AOF, RDB, replication, modules, memory efficiency, and
active defragmentation.

The same revision passed complete `unit/type/zset` and standalone and cluster
`unit/scan` runs under both AddressSanitizer and UndefinedBehaviorSanitizer.
The forced-defrag build passed the complete zset suite with the B+ tree
relocation test enabled. The mixed external-member stress test also completed
30,000 random operations followed by repeated RDB reloads in each of these
independent builds:

* AddressSanitizer with `DEBUG_ASSERTIONS`;
* UndefinedBehaviorSanitizer with `DEBUG_ASSERTIONS`; and
* `DEBUG_DEFRAG_FORCE` with `DEBUG_ASSERTIONS`.

The stress set cycles through binary members of 16, 253, 254, 319, 320, 321,
1024, and 4096 bytes. It compares complete score order, scores, and ranks with
an independent model while mixing insertion, updates, removal, range deletion,
pop, COPY, and reload.

## Current limitations

* The implementation adds a substantial private data structure. Until bulk
  commands stop using the skiplist, three zset encodings and every conversion
  between them must remain correct.
* Successful lookup depends on data placement. The primary short-member test
  is even for random lookup, 10.19% faster in score order, and 23.12% slower in
  original insertion order. The mixed-size test is 6.21% slower end to end and
  47.77% slower in Redis's per-command time.
* Building 200,000 random short members is 8.40% slower inside Redis, although
  the measured end-to-end difference is 4.28%. A deliberately ordered set with
  one shared score is 18.36% slower inside Redis and 10.23% slower end to end;
  that object is 73.45% smaller.
* `ZSCAN` is unordered and may return duplicates even without a write when
  several members in one leaf share the same 8 bit tag. During a member-table
  replacement the cursor restarts once after the new table is installed. A
  cursor also cannot be translated if an internal caller explicitly changes
  the object to another encoding. These behaviors are within SCAN's contract.
  In the 200,000-member benchmark the scan returned zero to eight extra entries
  and was 6.56% faster in Redis CPU time than the previous scanner.
* A member-table growth normally copies a bounded number of leaves per command.
  If the new table unexpectedly fills before copying finishes,
  `zbtIndexExpandIfNeeded()` completes the current copy before starting a
  larger table. This defensive path is linear in the number of live leaves and
  is a rare but real command-latency cliff.
* Deleting more than 1,024 members and at least one tenth of the set rebuilds
  the member table from the survivors. This makes measured large rank, score,
  and lex deletions much faster, but work can jump near that threshold and is
  linear in the number of survivors.
* Fork-page dismissal is effective only on Linux with jemalloc, matching the
  existing Redis dismissal mechanism.

## Remaining design work

The external-member change exposed an important standard for judging the
design. A local inconvenience should not force the whole sorted set into a
second representation or impose a permanent global cost. Applying that
standard to the complete implementation reveals the following opportunities.

### 1. Build B+ trees directly in bulk

`ZUNION`, `ZINTER`, and `ZDIFF` still create a skiplist as temporary storage.
A stored result is then converted into a listpack or B+ tree. The skiplist is
allocated, filled, and discarded even though it is not the desired result.

The direct path should collect the final member and score pairs, sort them once
when necessary, fill score leaves in order, build parent levels from those
leaves, and create the member index in one pass. The same builder can serve RDB
loading, listpack conversion, skiplist conversion, and `ZRANGESTORE`.

This is the largest clear design hole. Fixing it removes temporary allocations,
peak memory, random tree insertion, repeated leaf splitting, and conversion
work. Once all callers use it, the old skiplist can stop being a normal
construction tool and much of its dispatch and conversion code can eventually
be removed.

### 2. Use element counts that callers already know

RDB provides the exact element count. `ZRANGESTORE` often knows its result
length. `zsetConvertAndExpand()` receives a capacity argument and promises to
pre-size the result. The skiplist dictionary uses that argument; the B+ tree
ignores it and starts with an empty lazy index.

At minimum, the B+ tree needs a reserve operation for the member table and leaf
address table. The bulk builder is the better complete answer because it can
also choose the exact number of leaves and parent pages. This improves load and
conversion speed and avoids temporary old and new hash tables during initial
construction.

### 3. Make memory depend on current contents, not past size

Normal deletions merge small leaves and can shrink the member hash table, but
three parts can retain an old high-water mark:

* the leaf-address array grows by powers of two and does not normally shrink;
* a member table that changed from 16 to 32 bit leaf numbers stays wide; and
* empty inner nodes disappear, but partially filled inner nodes are not joined
  or rebuilt.

A set grown very large and then reduced by scattered deletions can therefore
use more memory than the same final members inserted into a new key. Large
range deletion already has code to renumber leaves and rebuild the member
index, but ordinary deletion history does not trigger equivalent repair.

The set should occasionally compare live leaves with allocated leaf slots,
largest leaf ID, table width, and inner-page occupancy. When the difference is
large, it can renumber leaves, rebuild the member table in its narrowest form,
and reconstruct only the small inner levels from the linked leaves. This must
be occasional work, not something done after every deletion.

### 4. Let the member index identify the exact member

One hash slot currently identifies a score leaf, not the member's record. A
lookup follows the leaf number and scans members with the same 8 bit tag. This
keeps the hash slot small and avoids repairing positions whenever a leaf is
rebuilt, but it explains the remaining successful-lookup regressions.

A compact record position or byte offset could make lookup direct. It must be
checked before use, and leaf rebuilding must repair affected entries. One
possible accounting is to add one position byte to each narrow hash slot and
remove the separate tag byte currently stored for every leaf member. That is
close to memory-neutral, but the write cost must be measured because positions
inside a packed leaf change.

Another option is a validated hint: try the saved position first and scan the
leaf only when it became stale. This reduces repair work but does not guarantee
the faster path. Both forms need a prototype and mixed read/write benchmarks;
this is a promising design change, not yet a proven win.

### 5. Avoid widening every hash bucket at once

Most member-index buckets are 32 bytes: two 8-byte tag words and eight 16 bit
leaf numbers. A very large set changes every bucket to a 48-byte form with 32
bit leaf numbers. The change begins early enough to finish incremental copying
before 16 bit IDs can run out.

Before widening, the implementation should renumber released leaf IDs. A
packed 24-bit number is another possibility and would cover far larger sets
without increasing each ID to four bytes. Both changes matter only for very
large sorted sets, but the present all-at-once 50% bucket increase is an
avoidable boundary.

### 6. Keep unusual scores from widening a complete leaf

Every score in one leaf uses the same base, shift, and bit width. One score with
unusual low bits or a distant value can make all scores in that leaf wider.

A small exception bitmap could store common scores in the compact form and a
few unusual scores as complete doubles. Small independent score groups inside
a leaf are another option. Either approach adds branches and metadata, so the
next step is to measure real score distributions and the frequency and cost of
widened leaves before changing the format.

### 7. Reduce peak memory while the member table grows

Incremental resizing spreads CPU work over commands, but the complete new
table is allocated before the old one is released. Near a growth boundary the
member index therefore consumes close to both sizes at once.

A table made of independently allocated segments could grow without a second
complete array. This would make peak memory more proportional to live data,
but changes lookup and scan behavior and adds real complexity. It belongs
after the simpler reserve, bulk-build, and history-compaction work.

### 8. Revisit the listpack decision using total bytes

Listpack selection uses element count and the largest member, with a separate
safety check where total bytes are available. A tiny set can therefore leave
the listpack because one member barely exceeds the configured per-member limit
even when the complete listpack would remain small.

A count limit, total-byte budget, and hard very-large-member limit describe the
actual cost more directly. This affects small sorted sets rather than the main
large-set design and should remain lower priority.

## What is not a design hole

Sorted sets support two different searches: direct lookup by member and order
by `(score, member)`. Keeping two logical indexes is the straightforward way to
retain expected constant-time member lookup and logarithmic rank and range
operations. The important property is that both indexes refer to one owned
member value; duplicating the payload is unnecessary.

External allocation for a 320-byte-or-larger member is also deliberate. It
applies only to that member, gives it one owner, and prevents leaf maintenance
from copying it. Making the threshold configurable would add compatibility and
test surface without changing observable behavior, so the measured fixed value
is preferable.

Arenas and pointer replacement by integer offsets remain possible future
experiments. They are not needed for the current result and would make active
defragmentation, ownership, and allocator accounting more invasive.

## Suggested order

1. Add B+ tree reservation and a bottom-up bulk builder.
2. Use the builder for RDB, conversions, `ZRANGESTORE`, and set operations.
3. Remove temporary skiplist construction and reassess whether the skiplist
   encoding still needs to remain reachable.
4. Add occasional leaf-ID, hash-width, and inner-level compaction after
   scattered deletions.
5. Prototype an exact compact member location and keep it only if lookup speed
   improves without losing the memory result or write speed.
6. Measure score-width waste and hash-resize peaks before considering the more
   complex formats.

## Recommendation

The implementation is a strong default large-zset representation. On the main
short-member workload it reduces object memory by about 61%, retains a 54%
saving after extensive score changes, and materially accelerates rank, missing
lookups, score updates, and large deletions. External member records close the
major hole that previously retained the skiplist solely because one member was
large.

The cost is about 4,467 net production lines, slower construction in measured
cases, successful-lookup regressions for some memory layouts, and two uncommon
linear-time maintenance paths described above. The exact final source passes
the full release suite plus focused ASAN, UBSAN, and forced-defrag QA, and two
independent reviews found no remaining blocker. Normal project CI must still
cover platforms not available in this pass, especially Linux x86-64 and any
supported 32-bit or big-endian build.

Bulk construction and history-independent compaction are the highest value
follow-up changes. The exact-member hash location is the most promising
performance experiment after those structural issues are removed.
