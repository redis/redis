# Native Bitmap RDB Payload Format

This note documents the Redis-owned native bitmap payload written under
`RDB_TYPE_BITMAP` in this fork. It is intended for Redis, librdb, and external
RDB/DUMP tooling; parsers do not need to link against CRoaring to identify the
payload shape or inspect its containers.

## Top-Level Framing

`RDB_TYPE_BITMAP` writes the final container stream directly:

1. `byte_len`: logical bitmap byte length as an RDB length integer.
2. `container_count`: number of 2^16-bit containers as an RDB length integer.
3. `container_count` records in increasing `high48` order:
   - `high48`: high 48 bits of the set-bit offsets in this container.
   - `typecode`: `1` bitset, `2` array, or `3` run.
   - `cardinality`: number of set bits in the container.
   - `payload`: RDB raw string containing the container body.

All multi-byte numbers inside a container `payload` are little-endian,
independent of host architecture. The surrounding RDB length integers keep the
normal Redis RDB integer encoding.

Container payloads:

| Typecode | Payload |
| ---: | --- |
| `1` bitset | `1024` little-endian `uint64_t` words, covering 65536 bits. |
| `2` array | `cardinality` little-endian `uint16_t` bit positions, strictly increasing. |
| `3` run | Little-endian `uint16_t run_count`, then `run_count` pairs of little-endian `uint16_t start` and `uint16_t length`; `length` is inclusive, so a single-bit run has length `0`. Runs must be sorted, non-overlapping, and non-adjacent. |

## Validation

The loader rejects malformed payloads before exposing an object:

- `byte_len` must fit the native bitmap cap (`512 MiB` in the current v1
  bounded surface).
- `container_count` must not exceed `ceil(byte_len / 8192)`.
- Container `high48` values must be strictly increasing.
- Typecodes are limited to array, bitset, and run; CRoaring shared-container
  wrappers are never serialized.
- Container payload lengths are bounded before allocation, then checked against
  the exact length for their type.
- Cardinality must match the decoded container contents.
- The highest set bit must be less than `byte_len * 8`.
- Structural validation rejects unsorted arrays and invalid run layouts.
