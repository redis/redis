# Native Bitmap RDB Payload Format

This note documents the Redis-owned native bitmap payload written under
`RDB_TYPE_BITMAP` in this fork. It is intended for Redis, librdb, and external
RDB/DUMP tooling; parsers do not need to link against CRoaring or understand
CRoaring's private serialization format.

## Top-Level Framing

`RDB_TYPE_BITMAP` writes the observable bitmap bytes directly:

1. `byte_len`: logical bitmap byte length as an RDB length integer.
2. `raw`: an RDB raw string whose decoded length must equal `byte_len`.

The `raw` string is exactly the legacy Redis bitmap byte representation:
bit offset `0` is the most significant bit of byte `0`, bit offset `7` is the
least significant bit of byte `0`, and so on. Normal Redis string RDB encodings
can still apply to the raw string, including integer encoding for short numeric
byte sequences and LZF compression when enabled.

There is no native-bitmap-specific encoding tag or draft compatibility marker in
the payload. Earlier in-PR raw/range/container/portable draft formats were never
released, so the loader rejects them instead of preserving load-only branches.

## Validation

The loader rejects malformed payloads before exposing an object:

- `byte_len` must fit the native bitmap cap (`512 MiB` in the current v1
  bounded surface).
- The decoded raw string length must equal `byte_len`.
- Unknown Redis string encodings, truncated raw strings, malformed LZF strings,
  and mismatched lengths fail the load/RESTORE operation.

After validation, Redis rebuilds the internal Roaring representation from the
raw bitmap bytes. The on-disk payload therefore stays inspectable and independent
of CRoaring internals while keeping the format simple for v1.
