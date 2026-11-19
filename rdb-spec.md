<!--
Copyright (c) 2009-Present, Redis Ltd.
All rights reserved.

Licensed under your choice of (a) the Redis Source Available License 2.0
(RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
GNU Affero General Public License v3 (AGPLv3).
-->

## Redis RDB file specification (RDB_VERSION 12)

This is a wire/storage specification. Writers may choose different internal encodings
so long as they produce the exact byte layout specified below; readers must accept
all encodings defined here.


### RDB Version and header
- Magic header: ASCII "REDIS%04d" where %04d is a zero‑padded decimal RDB version.
  Current version is 12, so header bytes are: R E D I S 0 0 1 2.
- After the header, a sequence of opcodes follows, typically starting with auxiliary metadata
  (AUX fields, module auxiliary data, function libraries), then database records with key/value
  pairs (see "Top‑level structure").
- End of file is marked by opcode RDB_OPCODE_EOF (255), followed by an 8‑byte CRC64
  checksum of all prior bytes in the file. The checksum is stored little‑endian. When checksums are disabled at save time, a zero checksum may be stored and loaders will skip validation.

  
### RDB Top‑level structure
After the header, the file is a stream of records and opcodes until EOF:
1) Zero or more AUX records (RDB_OPCODE_AUX) — arbitrary key/value metadata (see “AUX fields”).
2) Zero or more MODULE_AUX blocks (RDB_OPCODE_MODULE_AUX) — module metadata not tied to keys. Writers here save two groups: BEFORE_RDB and AFTER_RDB.
3) Zero or more function library entries (RDB_OPCODE_FUNCTION2) — see “Function libraries”.
4) For each non‑empty logical database:
    - SELECTDB (RDB_OPCODE_SELECTDB), then dbid (length‑encoded)
    - RESIZEDB (RDB_OPCODE_RESIZEDB), then db_size and expires_size (length‑encoded hints)
    - Optionally, when cluster mode is enabled, repeated SLOT_INFO (RDB_OPCODE_SLOT_INFO) triplets can appear to hint per‑slot sizes: slot_id, slot_keys, slot_expiring_keys (all length‑encoded). Loaders not in cluster mode ignore them.
    - Then, a sequence of key records, each consisting of optional key‑attribute opcodes followed by a value record:
      - Optional EXPIRETIME_MS (RDB_OPCODE_EXPIRETIME_MS), then 8‑byte little‑endian ms epoch
      (legacy EXPIRETIME in seconds also exists: RDB_OPCODE_EXPIRETIME + 4‑byte seconds)
      - Optional IDLE (RDB_OPCODE_IDLE), then a length‑encoded idle time in seconds (LRU)
      - Optional FREQ (RDB_OPCODE_FREQ), then a single byte 0..255 (LFU)
      - Then an object TYPE byte (one of RDB_TYPE_*)
      - Then the key name as a string (see length encodings)
      - Then the value payload according to the TYPE
5) EOF (RDB_OPCODE_EOF) and the CRC64 checksum.

Ordering notes:
- The attribute opcodes (expire/idle/freq) apply to the immediately following key only; they may appear in any order (writers usually emit expire, then idle, then freq).
- AUX and MODULE_AUX may appear before and/or after the keyspace; loaders accept them anywhere in the stream prior to EOF.


### Object types
Numeric IDs are defined in src/rdb.h. Current set (version 12):
- RDB_TYPE_STRING (0)
- RDB_TYPE_LIST (1) [legacy, ziplist‑based; writer no longer emits]
- RDB_TYPE_SET (2)
- RDB_TYPE_ZSET (3) — textual double scores
- RDB_TYPE_HASH (4)
- RDB_TYPE_ZSET_2 (5) — binary double scores
- RDB_TYPE_MODULE_PRE_GA (6) [unsupported on load]
- RDB_TYPE_MODULE_2 (7)
- RDB_TYPE_HASH_ZIPMAP (9) [legacy]
- RDB_TYPE_LIST_ZIPLIST (10) [legacy]
- RDB_TYPE_SET_INTSET (11)
- RDB_TYPE_ZSET_ZIPLIST (12) [legacy]
- RDB_TYPE_HASH_ZIPLIST (13) [legacy]
- RDB_TYPE_LIST_QUICKLIST (14) [v1; still accepted on load]
- RDB_TYPE_STREAM_LISTPACKS (15)
- RDB_TYPE_HASH_LISTPACK (16)
- RDB_TYPE_ZSET_LISTPACK (17)
- RDB_TYPE_LIST_QUICKLIST_2 (18) [current list format]
- RDB_TYPE_STREAM_LISTPACKS_2 (19)
- RDB_TYPE_SET_LISTPACK (20)
- RDB_TYPE_STREAM_LISTPACKS_3 (21)
- RDB_TYPE_HASH_METADATA_PRE_GA (22) [field TTLs, absolute, no min header]
- RDB_TYPE_HASH_LISTPACK_EX_PRE_GA (23) [field TTLs, absolute, no min header]
- RDB_TYPE_HASH_METADATA (24) [field TTLs, relative to min header]
- RDB_TYPE_HASH_LISTPACK_EX (25) [field TTLs, relative to min header]

A type is followed by a type‑specific payload as described below.


### Integer and length encodings
Many fields use a compact length encoding with the top 2 bits of the first byte indicating the format (see rdb.h):
- 00 | xxxxxx        → 6‑bit length in the low 6 bits (0..63), single byte total
- 01 | xxxxxx yyyyyyyy → 14‑bit length across two bytes (0..16383)
- 10 | 000000 [32‑bit big‑endian] → full 32‑bit length (network byte order)
- 10 | 000001 [64‑bit big‑endian] → full 64‑bit length (network byte order)
- 11 | enc           → "special encoding" marker; the low 6 bits give the special encoding type (see below)

Special string encodings (first byte has top bits 11):
- RDB_ENC_INT8  (0): the string is a single signed 8‑bit integer that follows
- RDB_ENC_INT16 (1): the string is a signed 16‑bit little‑endian integer
- RDB_ENC_INT32 (2): the string is a signed 32‑bit little‑endian integer
- RDB_ENC_LZF   (3): the string is LZF‑compressed; two length fields follow (both length‑encoded as above): compressed_length, original_length; then compressed payload (see lzf.h)

Unless one of the special encodings is used, strings are stored as: length (per above) followed by that many bytes verbatim.

Times and binary floats/doubles:
- Millisecond times are stored as signed 64‑bit little‑endian integers. Since RDB v9, loaders convert endianness correctly; v9+ files use little‑endian on disk.
- Binary double and float encodings are stored little‑endian (8 or 4 bytes respectively).
- A legacy textual double encoding also exists (see ZSET v1 below): one length byte precedes an ASCII representation; special length values: 253=NaN, 254=+inf, 255=−inf.


#### Strings (RDB_TYPE_STRING)
- Payload is a string value using the string encodings described earlier. Writers may use:
  - Integer encodings (INT8/16/32) for small integer‑like payloads
  - LZF compression (RDB_ENC_LZF) for compressible long strings (typically >20 bytes)
  - Raw strings otherwise


#### Lists (RDB_TYPE_LIST_QUICKLIST_2 and RDB_TYPE_LIST_QUICKLIST)
- RDB_TYPE_LIST_QUICKLIST_2 (current):
    1) length‑encoded number of quicklist nodes
    2) For each node:
       - container (length‑encoded), matches quicklist container type constants
       - data: either an RDB_ENC_LZF blob (see LZF format above) if node is compressed, or a raw string containing the node’s payload
       - For PACKED nodes the payload is a listpack blob
    3) Writers may serialize a single‑listpack list as a "fake" quicklist with one PACKED node containing the listpack.
- RDB_TYPE_LIST_QUICKLIST (legacy v1):
    - Similar, but without the explicit container field per node. Loaders still accept it and convert to the current structures.
- RDB_TYPE_LIST (older legacy): ziplist encoding only; accepted on load, not emitted.


#### Sets (RDB_TYPE_SET, RDB_TYPE_SET_INTSET, RDB_TYPE_SET_LISTPACK)
- RDB_TYPE_SET: length (#elements), then N members as strings
- RDB_TYPE_SET_INTSET: a single binary intset blob
- RDB_TYPE_SET_LISTPACK: a single listpack blob containing the set elements


#### Sorted sets (RDB_TYPE_ZSET, RDB_TYPE_ZSET_2, RDB_TYPE_ZSET_ZIPLIST, RDB_TYPE_ZSET_LISTPACK)
- RDB_TYPE_ZSET: length (#elements), then N pairs of:
  - member (string)
  - score encoded as legacy textual double (see “Integer and length encodings”)
- RDB_TYPE_ZSET_2: same, but scores are 8‑byte little‑endian IEEE754 doubles
- RDB_TYPE_ZSET_ZIPLIST: a single ziplist blob (legacy)
- RDB_TYPE_ZSET_LISTPACK: a single listpack blob


#### Hashes without per‑field TTL (RDB_TYPE_HASH, RDB_TYPE_HASH_LISTPACK)
- RDB_TYPE_HASH: length (#fields), then that many [field][value] string pairs
- RDB_TYPE_HASH_LISTPACK: a single listpack blob with [field][value] pairs


#### Hashes with per‑field TTL (HFEs)
Two encodings exist, each with a PRE_GA legacy variant:

- RDB_TYPE_HASH_METADATA (hashtable with metadata):
    1) minExpire header: 8‑byte little‑endian ms epoch (0 if none); this is the minimal non‑zero expiration time among all fields
    2) length (#fields)
    3) For each field:
       - ttl: length‑encoded integer; 0 means “no TTL”; otherwise ttl_value = expireAt − minExpire + 1 (kept relative to keep small)
       - field (string)
       - value (string)

- RDB_TYPE_HASH_LISTPACK_EX (listpack with TTLs):
    1) minExpire header: 8‑byte little‑endian ms epoch (0 if none)
    2) listpack blob whose tuples are [field][value][ttl], where ttl uses the same convention as above (0 or relative +1)

Legacy PRE_GA variants (RDB_TYPE_HASH_METADATA_PRE_GA and RDB_TYPE_HASH_LISTPACK_EX_PRE_GA) do not include the minExpire header and store TTLs as absolute expireAt millisecond timestamps inside the stream of tuples.


#### Streams (RDB_TYPE_STREAM_LISTPACKS, _2, _3)
Stream payload consists of:
1) length‑encoded count of listpack nodes in the radix tree
2) For each node:
   - 16‑byte streamID key, stored as the raw struct bytes (big‑endian order as used in the radix tree)
   - listpack blob with the node’s entries
3) Stream metadata:
   - length (total items)
   - last_id.ms (len‑encoded), last_id.seq (len‑encoded)
   - For v2+ (RDB_TYPE_STREAM_LISTPACKS_2 and _3): first_id.ms, first_id.seq; max_deleted_entry_id.ms, max_deleted_entry_id.seq; entries_added (offset)
4) Consumer groups:
   - length‑encoded number of groups
   - For each group: name (string), last_id.ms, last_id.seq, entries_read (logical read counter). For v2+ also a length‑encoded group offset; for v1 loaders approximate it.
   - Global PEL (pending entries list) for the group: a count, then for each entry:
   – 16‑byte raw streamID; delivery_time (8‑byte little‑endian ms); delivery_count (length‑encoded)
   - Consumers: a count, then for each consumer:
   – name (string); seen_time (8‑byte little‑endian ms); for v3 also active_time (8‑byte little‑endian ms)
   – Consumer PEL: count, then that many raw 16‑byte streamIDs (no NACK bodies; they reference the group PEL entries by ID)


#### Modules (RDB_TYPE_MODULE_2)
- moduleid: length‑encoded unsigned identifying the module type and its serialization version (lower 10 bits are the version)
- Then a module‑specific payload as a sequence of sub‑opcodes and data (all length‑encoded unless noted):
  - RDB_MODULE_OPCODE_SINT / UINT: signed/unsigned integers (length‑encoded)
  - RDB_MODULE_OPCODE_FLOAT / DOUBLE: 4/8‑byte little‑endian IEEE754
  - RDB_MODULE_OPCODE_STRING: an RDB string (may itself use special encodings)
- The payload is terminated by RDB_MODULE_OPCODE_EOF (0)
- RDB_TYPE_MODULE_PRE_GA (6) is a pre‑release format and is rejected by the loader


### AUX fields (RDB_OPCODE_AUX)
- Format: opcode (250), then key (string), then value (string)
- Unknown AUX keys must be ignored by loaders (forward‑compatible extension point)
- Writers currently emit, among others: redis‑ver, redis‑bits, ctime, used‑mem, repl‑stream‑db, repl‑id, repl‑offset, aof‑base. Keys starting with ‘%’ are informational and may be logged at NOTICE level when loading.


### Module AUX blocks (RDB_OPCODE_MODULE_AUX)
- Format:
    1) opcode (247)
    2) moduleid (length‑encoded)
    3) RDB_MODULE_OPCODE_UINT (sub‑opcode) then ‘when’ (length‑encoded) — indicates BEFORE_RDB or AFTER_RDB, etc.
    4) module‑specific aux payload (module decides its own sub‑opcodes/data)
    5) RDB_MODULE_OPCODE_EOF terminator
- Unknown modules or modules lacking aux_load are treated as fatal during loading (outside rdb‑check mode).


### Function libraries (RDB_OPCODE_FUNCTION2)
- Each library is encoded as:
  - opcode (245)
  - a single string containing the library code payload (the engine and metadata are derived by the loader)
- These entries typically appear after AUX and before the keyspace. The same structure is also used by the FUNCTION DUMP/RESTORE feature; in that context, a trailing header (RDB version) and CRC64 may be appended to the payload for self‑validation.


### Database selection and size hints
- RDB_OPCODE_SELECTDB (254): followed by dbid (length‑encoded). Must appear before any keys of that db.
- RDB_OPCODE_RESIZEDB (251): two length‑encoded values: db_size (total keys) and expires_size (keys with TTL). Loaders may use these to pre‑size hash tables.
- RDB_OPCODE_SLOT_INFO (244): in cluster mode only, a triple of length‑encoded slot_id, slot_size, expires_slot_size, which may repeat as the writer iterates slots. Non‑cluster loaders ignore these hints.


### Key attributes
- RDB_OPCODE_EXPIRETIME_MS (252): 8‑byte little‑endian ms epoch; applies to the next key only
- RDB_OPCODE_EXPIRETIME (253): legacy seconds precision (4‑byte); still accepted on load
- RDB_OPCODE_IDLE (248): a length‑encoded idle time in seconds (LRU)
- RDB_OPCODE_FREQ (249): a single byte 0..255 (LFU)


### EOF and checksum
- RDB_OPCODE_EOF (255) marks the end of the stream.
- An 8‑byte little‑endian CRC64 (poly as used by Redis) follows. A value of 0 indicates checksums were disabled during save.


### Streaming over the network (EOF‑mark variant)
When an RDB is streamed to replicas without a fixed length, writers may wrap the RDB in an EOF‑mark framing:
- Prefix: the ASCII line “$EOF:<40‑hex‑chars>\r\n”
- Body: the raw RDB stream as specified above
- Suffix: the same 40‑hex string again
  This allows the receiver to detect end of stream without parsing the RDB content.


### Legacy container formats
The loader accepts old encodings for backward compatibility. Writers in current versions do not emit them:
- zipmap (RDB_TYPE_HASH_ZIPMAP)
- ziplist list/zset/hash (RDB_TYPE_LIST_ZIPLIST, RDB_TYPE_ZSET_ZIPLIST, RDB_TYPE_HASH_ZIPLIST)
- quicklist v1 without per‑node container (RDB_TYPE_LIST_QUICKLIST)
- hash field TTL pre‑GA formats (RDB_TYPE_HASH_METADATA_PRE_GA, RDB_TYPE_HASH_LISTPACK_EX_PRE_GA)


### Notes on semantics
- Expired keys: When loading an RDB as a master (and not as an AOF preamble), keys already expired at load time may be dropped; replicas or AOF preamble loads keep them for deterministic replication.
- Eviction metadata: If present, IDLE and FREQ are applied to the loaded object’s LRU/LFU state.
- Hash Field Expirations (HFEs): When the minimal expiration time (minExpire) is present, TTLs in the payload are relative (ttl = expireAt − minExpire + 1) and 0 means no TTL. PRE_GA files used absolute per‑field expireAt without a min header.


### Appendix: opcode/type numeric values
For reference (see src/rdb.h):
- RDB types: 0..7, 9..25 inclusive (see list above)
- Special opcodes:
  - RDB_OPCODE_SLOT_INFO 244
  - RDB_OPCODE_FUNCTION2 245
  - RDB_OPCODE_FUNCTION_PRE_GA 246 [pre-release, rejected on load]
  - RDB_OPCODE_MODULE_AUX 247
  - RDB_OPCODE_IDLE 248
  - RDB_OPCODE_FREQ 249
  - RDB_OPCODE_AUX 250
  - RDB_OPCODE_RESIZEDB 251
  - RDB_OPCODE_EXPIRETIME_MS 252
  - RDB_OPCODE_EXPIRETIME 253
  - RDB_OPCODE_SELECTDB 254
  - RDB_OPCODE_EOF 255

This document is intentionally implementation‑driven; consult src/rdb.c and src/rdb.h for the authoritative source when evolving the format.
