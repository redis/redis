# redis-roaring Test Porting Ledger

Issue #57 tracks porting user scenarios from
`aviggiano/redis-roaring` into Redis native bitmap coverage without adding the
old `R.*` or `R64.*` module command surface to Redis core. This ledger records
how each source corpus item is represented in the Redis core tests.

## Status Legend

- **Direct**: covered by the same Redis core bitmap command behavior.
- **Translated**: covered by equivalent Redis core bitmap commands or set
  algebra over bitmap keys.
- **Existing**: covered by pre-existing Redis tests on this branch.
- **Out of scope**: intentionally not imported, with rationale.

## Source Corpus

| Source | Status | Redis coverage |
| --- | --- | --- |
| `README.md` API and examples | Direct / Translated | Standard bitmap commands are direct. Module-only set/list operations are translated in `tests/unit/bitmap-native-commands.tcl`. |
| `docs/commands/*.md` | Direct / Translated / Out of scope | Command families are mapped below. Command documentation itself is not copied because Redis core does not expose `R.*` or `R64.*`. |
| `tests/unit/*.c` | Direct / Translated | Core bitmap assertions cover exact set bits, algebra, boundary offsets, and native/string equivalence. |
| `tests/integration_1.sh` | Direct / Translated | User command flows are represented by bitmap-native unit tests and translated set-algebra tests. |
| `tests/integration_2.sh` | Existing | RDB reload behavior is covered by `tests/unit/bitmap-native-type.tcl` debug reload, `DUMP`/`RESTORE`, and full resync tests. |
| `tests/integration_3.sh` | Direct / Translated | 64-bit-prefixed module scenarios collapse to the single Redis native bitmap representation and the same core command coverage. |
| `tests/integration_4.sh` | Existing | Cluster same-slot `BITOP` behavior is covered by `tests/cluster/tests/00-base.tcl`. |
| `tests/fuzz/corpus/*` | Direct / Translated / Existing | Deterministic seed shapes are represented in `tests/unit/bitmap-native-oracle.tcl`, BITOP raw fuzz tests, corrupt dump fuzzing, and fuzzy traffic generation. |
| `docs/fuzzing.md` and `tests/fuzz/*` | Translated / Existing / Out of scope | Fuzz goals are mapped to Redis Tcl tests and existing fuzz-style helpers. The libFuzzer harnesses themselves are module-specific and are not imported. |
| `performance.c` and `performance.sh` | Out of scope | Benchmarks are explicitly excluded from correctness test porting. |

## Command Family Mapping

| redis-roaring family | Status | Redis core coverage |
| --- | --- | --- |
| `R.SETBIT`, `R64.SETBIT` | Direct | `SETBIT` parity across legacy strings and native bitmaps in `tests/unit/bitmap-native-oracle.tcl`; native write paths and offset limits in `tests/unit/bitmap-native-commands.tcl` and `tests/unit/bitmap-native-type.tcl`. |
| `R.GETBIT`, `R64.GETBIT` | Direct | `GETBIT` parity, reads past logical length, and boundary reads in bitmap-native command tests. |
| `R.GETBITS`, `R64.GETBITS` | Translated | Multi-bit reads are represented by repeated `GETBIT` and `BITFIELD_RO ... GET` assertions in command and oracle tests. |
| `R.CLEARBITS`, `R64.CLEARBITS` | Translated | Repeated `SETBIT offset 0` plus exact-bit and `BITCOUNT` assertions in `translated redis-roaring int-array bit-array and clear scenarios`. |
| `R.SETINTARRAY`, `R64.SETINTARRAY` | Translated | Seed helpers create exact bitsets through `SETBIT`; `assert_bitmap_has_exact_bits` verifies resulting cardinality and membership. |
| `R.GETINTARRAY`, `R64.GETINTARRAY` | Translated | Exact membership is verified by `assert_bitmap_has_exact_bits`; Redis core does not expose integer-array enumeration. |
| `R.RANGEINTARRAY`, `R64.RANGEINTARRAY` | Translated | Paging-style examples are represented by exact-bit fixtures, `BITFIELD_RO` sampled reads, `BITCOUNT ... BIT` ranges, and `BITPOS`. |
| `R.APPENDINTARRAY`, `R64.APPENDINTARRAY` | Translated | Additional integers are modeled as more `SETBIT offset 1` writes against an existing bitmap. |
| `R.DELETEINTARRAY`, `R64.DELETEINTARRAY` | Translated | Deletions are modeled as repeated `SETBIT offset 0`. |
| `R.SETBITARRAY`, `R64.SETBITARRAY` | Translated | Bit-array strings are represented by deterministic bit fixtures and sampled `BITFIELD_RO` reads. |
| `R.GETBITARRAY`, `R64.GETBITARRAY` | Translated | Core has no bit-array string reply; equivalent observable behavior is exact membership plus sampled bit reads. |
| `R.SETRANGE`, `R64.SETRANGE` | Translated | Bounded range examples are represented by repeated `SETBIT` and dense `BITFIELD SET` oracle seeds. |
| `R.SETFULL`, `R64.SETFULL` | Translated / Out of scope | Bounded all-ones surrogates are covered. Materializing the full 32-bit or 64-bit universe is not imported as a correctness test. |
| `R.CLEAR`, `R64.CLEAR` | Translated | Clearing all set bits is represented by exact empty bitsets after repeated clears; key deletion behavior remains normal Redis `DEL`. |
| `R.BITCOUNT`, `R64.BITCOUNT` | Direct | Full and ranged `BITCOUNT` parity is covered in native command tests and oracle tests. |
| `R.BITPOS`, `R64.BITPOS` | Direct | First-one, first-zero, empty, dense-run, and container-boundary cases are covered in native command tests. |
| `R.BITFIELD` | Direct | `BITFIELD` read/write/overflow parity and native direct paths are covered in native command tests. |
| `R.BITOP`, `R64.BITOP` | Direct | `AND`, `OR`, `XOR`, `NOT`, `DIFF`, `DIFF1`, `ANDOR`, and `ONE` are covered by native/string parity, mixed source tests, missing keys, duplicate sources, destination aliasing, wrong arity, wrong type cleanup, cluster same-slot tests, AOF/rewrite, and replication tests. |
| `R.DIFF`, `R64.DIFF` | Translated | The module command is represented by `BITOP DIFF` and exact-bit assertions. |
| `R.CONTAINS`, `R64.CONTAINS` | Translated | Intersection, subset, strict-subset, equality, empty, and disjoint cases are represented by `BITOP AND`, `BITOP DIFF`, `BITOP XOR`, and `BITCOUNT`. |
| `R.JACCARD`, `R64.JACCARD` | Translated | Jaccard scenarios are represented by `BITOP AND` / `BITOP OR` cardinality ratios, including empty-union handling. |
| `R.MIN`, `R64.MIN` | Translated | Minimum set bit is represented by `BITPOS key 1`; empty cases use `BITCOUNT == 0` and `BITPOS == -1`. |
| `R.MAX`, `R64.MAX` | Translated | Maximum set bit is represented by exact-bit fixtures and explicit highest-offset membership checks; Redis core has no max-bit enumeration command. |
| `R.OPTIMIZE`, `R64.OPTIMIZE` | Existing / Out of scope | Roaring container optimization is an internal encoding concern. Persistence, memory, digest, active defrag, and exact raw-byte tests verify observable stability. |
| `R.STAT` | Translated / Out of scope | User-observable pieces are represented by `TYPE`, `OBJECT ENCODING`, `BITCOUNT`, `BITPOS`, and `MEMORY USAGE`. Container-level reporting is not a Redis core user command. |
| Command metadata and key discovery | Existing | Redis command specs, key specs, cluster routing tests, and `generate_fuzzy_traffic_on_key` cover the Redis core command surface. Module-prefixed metadata is not imported. |

## Offset and Representation Notes

Redis core native bitmaps on this branch use 64-bit-capable Roaring internals
but keep v1 native values bounded to 512 MiB of logical bytes (max bit offset
`4294967295`). Normal client commands also honor `proto-max-bulk-len` when it
is lower. Tests therefore cover the observable boundaries on this branch:
allowed offsets at the active native/client limit, rejected offsets immediately
past it, no mutation after rejected native writes, and RDB rejection for native
payloads beyond the v1 cap.

The `R.*` and `R64.*` split in the module maps to one Redis core bitmap API.
Where module tests compare 32-bit and 64-bit command families, Redis core tests
compare legacy string bitmaps and native Roaring bitmaps instead, because that
is the compatibility boundary users can observe after this port.

## Fuzz and Persistence Coverage

The module's libFuzzer targets are not imported directly. Their behavioral
oracles are represented as follows:

| redis-roaring fuzz target | Redis core representation |
| --- | --- |
| `fuzz_bitmap_api`, `fuzz_bitmap64_api` | Exact-bit fixtures, boundary offsets, and oracle mode parity. |
| `fuzz_bitmap_operations` | BITOP algebra tables plus mixed string/native raw fuzz cases. |
| `fuzz_bitmap_serialization` | Native bitmap `DUMP`/`RESTORE`, RDB reload, corrupt dump fuzzing, and endian conversion tests. |
| `fuzz_bitop_keys`, `fuzz_command_metadata` | Redis command key specs and cluster routing tests for the core command surface. |
| `fuzz_command_dispatch` | Tcl command scenarios for syntax errors, arity errors, wrong types, and operation semantics. |
| `fuzz_cluster_routing` | `tests/cluster/tests/00-base.tcl` same-slot native bitmap `BITOP` coverage. |
| `fuzz_persistence_sequences` | RDB reload, AOF rewrite, replication, full resync, and conversion propagation tests in `tests/unit/bitmap-native-type.tcl`. |
| `fuzz_r_vs_r64_parity` | Legacy-string vs native-roaring oracle parity plus deterministic seed replays in `tests/unit/bitmap-native-oracle.tcl`. |

## Out-of-Scope Items

- Reintroducing `R.*` or `R64.*` commands in Redis core.
- Importing legacy redis-roaring module RDB payload compatibility.
- Porting module performance benchmarks as correctness tests.
- Importing module-specific libFuzzer harness binaries or hiredis harness code.
- Exposing module-only enumeration/stat commands when Redis core has no matching
  user-facing command.
