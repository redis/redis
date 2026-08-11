#!/usr/bin/env bash
#
# Generate initial seed corpora for the Redis core fuzz targets.
#
# These are generator inputs, not RESP payloads. Bytes select command counts,
# grammar branches, keys, offsets, values, arities, encodings, and modes.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CORPUS_DIR="$SCRIPT_DIR/corpus"
STRING_DIR="$CORPUS_DIR/string_commands"
BITMAP_DIR="$CORPUS_DIR/bitmap_commands"
COMMAND_EXT_DIR="$CORPUS_DIR/command_extensions"
HASH_DIR="$CORPUS_DIR/hash_templates"
ARRAY_DIR="$CORPUS_DIR/array_commands"
REPLICATION_COMPRESSION_DIR="$CORPUS_DIR/replication_compression"
LIST_MOVE_DIR="$CORPUS_DIR/list_move_commands"
STREAM_DIR="$CORPUS_DIR/stream_commands"

mkdir -p "$STRING_DIR" "$BITMAP_DIR" "$COMMAND_EXT_DIR" "$HASH_DIR" "$ARRAY_DIR" "$REPLICATION_COMPRESSION_DIR" "$LIST_MOVE_DIR" "$STREAM_DIR"
rm -f "$STRING_DIR"/seed* "$BITMAP_DIR"/seed* \
    "$COMMAND_EXT_DIR"/seed* "$HASH_DIR"/seed* "$ARRAY_DIR"/seed* \
    "$REPLICATION_COMPRESSION_DIR"/seed* "$LIST_MOVE_DIR"/seed* "$STREAM_DIR"/seed*

echo "Generating Redis core fuzz seed corpora..."

# String command seeds.
# Multi-command stateful path: SET k0 "a"; APPEND k0 "b"; GET k0.
printf '\x02\x00\x00\x01a\x02\x00\x01b\x01\x00' \
    > "$STRING_DIR/seed01_set_append_get"

# SETRANGE k0 0 "xyz".
printf '\x00\x03\x00\x01\x03\x03xyz' \
    > "$STRING_DIR/seed02_setrange"

# GETRANGE k0 0 7.
printf '\x00\x04\x00\x01\x03\x01\x05' \
    > "$STRING_DIR/seed03_getrange"

# STRLEN k0.
printf '\x00\x05\x00' \
    > "$STRING_DIR/seed04_strlen"

# INCRBY k0 1.
printf '\x00\x06\x00\x01\x04' \
    > "$STRING_DIR/seed05_incrby"

# Invalid GETRANGE arity shape.
printf '\x00\x07\x00' \
    > "$STRING_DIR/seed06_invalid_getrange_shape"

# Bitmap command seeds.
# Multi-command stateful path: SETBIT k0 7 1; GETBIT k0 7.
printf '\x01\x00\x00\x01\x05\x01\x01\x01\x00\x01\x05' \
    > "$BITMAP_DIR/seed01_setbit_getbit"

# BITCOUNT k0 without a range.
printf '\x00\x02\x00\x00' \
    > "$BITMAP_DIR/seed02_bitcount_full"

# BITCOUNT k0 0 7 BYTE.
printf '\x00\x02\x01\x01\x00\x01\x03\x01\x05\x01' \
    > "$BITMAP_DIR/seed03_bitcount_byte_range"

# BITCOUNT k0 0 7 BIT.
printf '\x00\x02\x01\x01\x00\x01\x03\x01\x05\x00' \
    > "$BITMAP_DIR/seed04_bitcount_bit_range"

# BITPOS k0 1 0 7 BYTE.
printf '\x00\x03\x03\x00\x01\x01\x01\x03\x01\x05\x01' \
    > "$BITMAP_DIR/seed05_bitpos_byte_range"

# BITOP AND dst k1 k2.
printf '\x00\x04\x01\x00\x06\x01\x02' \
    > "$BITMAP_DIR/seed06_bitop_and"

# BITOP NOT dst k1.
printf '\x00\x04\x00\x03\x06\x01' \
    > "$BITMAP_DIR/seed07_bitop_not"

# Multi-command aliasing path: SETBIT k0; SETBIT k1; BITOP AND k0 k0 k1.
printf '\x02\x00\x00\x01\x05\x01\x01\x00\x01\x01\x06\x01\x01\x04\x01\x00\x00\x00\x01' \
    > "$BITMAP_DIR/seed08_bitop_dest_source_alias"

# BITOP badop dst k1.
printf '\x00\x04\x00\x06\x06\x01' \
    > "$BITMAP_DIR/seed09_bitop_invalid_op"

# BITFIELD k0 GET u8 7.
printf '\x00\x05\x00\x00\x00\x03\x01\x05' \
    > "$BITMAP_DIR/seed10_bitfield_get"

# BITFIELD k0 SET u8 7 1.
printf '\x00\x05\x00\x01\x00\x03\x01\x05\x01\x04' \
    > "$BITMAP_DIR/seed11_bitfield_set"

# BITFIELD k0 INCRBY u8 7 1.
printf '\x00\x05\x00\x02\x00\x03\x01\x05\x01\x04' \
    > "$BITMAP_DIR/seed12_bitfield_incrby"

# BITFIELD k0 OVERFLOW SAT INCRBY u8 7 1.
printf '\x00\x05\x01\x03\x02\x00\x01\x03\x01\x05\x01\x04' \
    > "$BITMAP_DIR/seed13_bitfield_overflow_incrby"

# BITFIELD_RO k0 GET u8 7.
printf '\x00\x06\x00\x00\x00\x03\x01\x05' \
    > "$BITMAP_DIR/seed14_bitfield_ro_get"

# Invalid BITOP arity shape.
printf '\x00\x07\x00' \
    > "$BITMAP_DIR/seed15_invalid_bitop_shape"

# Command-extension seeds.
# INCREX BYINT with reordered bounds, saturation, expiration, and ENX.
printf '\x00\x01\x06\x00\x02\x03\x08\x04\x09\x00\x01\x0d\x01\x02\x01\x0b\x02' \
    > "$COMMAND_EXT_DIR/seed01_increx_byint_bounds_expiry"

# Duplicate BYFLOAT plus NaN and infinity bounds.
printf '\x00\x01\x05\x01\x01\x02\x03\x08\x00\x01\x11\x01\x10\x01\x12\x01\x11' \
    > "$COMMAND_EXT_DIR/seed02_increx_float_specials"

# Conflicting PERSIST, absolute expiration options, and ENX ordering.
printf '\x00\x01\x04\x0a\x06\x07\x09\x01\x08\x08' \
    > "$COMMAND_EXT_DIR/seed03_increx_expiry_conflicts"

# Stateful SADDs followed by SUNIONCARD APPROX LIMIT.
printf '\x03\x02\x01\x00\x01\x05\x01\x07\x02\x01\x01\x01\x07\x01\x06\x02\x01\x02\x01\x06\x01\x08\x03\x02\x03\x00\x01\x02\x02' \
    > "$COMMAND_EXT_DIR/seed04_sunioncard_stateful"

# Large-first intset SDIFFCARD path, including LIMIT and SUNIONCARD APPROX.
printf '\x00\x08\x00\x05\x02' \
    > "$COMMAND_EXT_DIR/seed05_sdiffcard_intset_shape"

# Large-first string/hashtable SDIFFCARD path.
printf '\x00\x08\x01\x01\x07' \
    > "$COMMAND_EXT_DIR/seed06_sdiffcard_hashtable_shape"

# Weighted ZUNION COUNT reads and an aliased ZUNIONSTORE destination.
printf '\x03\x05\x01\x00\x01\x07\x01\x05\x01\x09\x01\x06\x05\x01\x01\x01\x09\x01\x05\x01\x07\x01\x07\x06\x00\x01\x07\x01\x00\x01\x06\x01\x06\x02\x01\x01\x01\x00\x00\x01\x06\x01' \
    > "$COMMAND_EXT_DIR/seed07_zunion_count_alias"

# Mixed set/zset ZINTER COUNT with weights and WITHSCORES.
printf '\x03\x02\x01\x00\x01\x05\x01\x06\x05\x01\x01\x01\x07\x01\x05\x01\x09\x01\x08\x02\x01\x02\x01\x05\x01\x07\x06\x01\x02\x03\x01\x00\x01\x02\x00\x03\x07' \
    > "$COMMAND_EXT_DIR/seed08_zinter_count_mixed_inputs"

# Wrong-type set and sorted-set extension calls.
printf '\x02\x00\x00\x00\x01\x05\x03\x00\x02\x00\x02\x06\x02\x00\x00\x01\x04\x00' \
    > "$COMMAND_EXT_DIR/seed09_wrong_types"

# Malformed extension-command arity and empty arguments.
printf '\x00\x09\x05\x06' \
    > "$COMMAND_EXT_DIR/seed10_malformed_shape"

# Existing LLONG_MAX followed by overflowing INCREX BYINT.
printf '\x01\x00\x00\x01\x01\x0d\x01\x01\x00\x00\x01\x07' \
    > "$COMMAND_EXT_DIR/seed11_increx_integer_overflow"

# Valid BYFLOAT with lower/upper bounds and saturation.
printf '\x01\x00\x00\x01\x01\x0a\x01\x04\x01\x02\x03\x08\x00\x01\x08\x01\x02\x01\x0b' \
    > "$COMMAND_EXT_DIR/seed12_increx_byfloat_bounds"

# Duplicate SUNIONCARD and SDIFFCARD LIMIT options.
printf '\x01\x03\x01\x06\x00\x01\x02\x03\x04\x01\x05\x00\x01\x02\x03' \
    > "$COMMAND_EXT_DIR/seed13_cardinality_duplicate_limits"

# Minimized UBSan reproducer: INCREX dst PX LLONG_MAX overflows when the
# relative TTL is converted to an absolute timestamp.
printf '\x00\x01\x01\x05\x04\x09' \
    > "$COMMAND_EXT_DIR/seed14_repro_increx_px_overflow"

# Minimized UBSan reproducer: malformed ZUNION with LLONG_MAX input keys
# overflows key-spec last-key arithmetic before normal syntax validation.
printf '\x00\x09\x02\x03\x01\x01\x0d' \
    > "$COMMAND_EXT_DIR/seed15_repro_zunion_keynum_overflow"

# Compact-hash / HIMPORT seeds.
# Shared, reordered fieldsets using template-listpack, followed by DUMP.
printf '\x00\x00\x00\x01b\x01c\x01A\x01B\x01C\x01D\x00\x0d\x04' \
    > "$HASH_DIR/seed01_shared_template_listpack"

# The same shared schema with hash-max-listpack-entries=0, forcing
# template-array, followed by HGETALL.
printf '\x01\x00\x00\x01b\x01c\x01A\x01B\x01C\x01D\x00\x0b\x05' \
    > "$HASH_DIR/seed02_shared_template_array"

# Duplicate PREPARE must leave the old fieldset binding intact.
printf '\x00\x00\x00\x01b\x01c\x01A\x01B\x01C\x01D\x00\x02\x00\x03dup\x01x' \
    > "$HASH_DIR/seed03_duplicate_schema"

# DISCARD, failed use, reordered re-PREPARE, SET, and DISCARDALL exercise
# holder/key-reference lifetimes.
printf '\x00\x00\x00\x01b\x01c\x01A\x01B\x01C\x01D\x04\x05\x00\x03\x00\x00\x00\x01x\x01\x01\x00\x03\x00\x00\x01W\x01X\x01Y\x01Z\x06' \
    > "$HASH_DIR/seed04_discard_reprepare_lifetime"

# COPY a template key, DUMP the copy, then mutate an existing field via HSET.
printf '\x00\x00\x00\x01b\x01c\x01A\x01B\x01C\x01D\x02\x0c\x04\x00\x01\x0d\x00\x08\x00\x00\x02\x01Z' \
    > "$HASH_DIR/seed05_copy_dump_hset"

# Long and binary field/value data cross the listpack-value threshold.
{
    printf '\x00\x00\x00'
    printf '\x04A\x00B\xff'
    printf '\x50'
    printf 'qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq'
    printf '\x50'
    printf 'vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv'
    printf '\x01B\x01C\x01D'
    printf '\x00\x0d\x04'
} > "$HASH_DIR/seed06_long_binary_fields_values"

# Invalid HIMPORT shapes and subcommands remain parser-safe.
printf '\x00\x00\x00\x01b\x01c\x01A\x01B\x01C\x01D\x04\x0f\x00\x0f\x01\x0f\x02\x00\x0f\x03\x0f\x04\x00\x00' \
    > "$HASH_DIR/seed07_malformed_himport"

# RESET drops all fieldsets; a SET through an old binding must fail, while a
# freshly prepared binding on the reset connection must work.
printf '\x00\x00\x00\x01b\x01c\x01A\x01B\x01C\x01D\x03\x07\x03\x00\x00\x00\x01Z\x00\x00\x00\x00\x03\x00\x00\x01V' \
    > "$HASH_DIR/seed08_reset_fieldsets"

# Array command seeds.
# ARSET k0 0 with 12 values promotes a populated slice to its dense encoding.
printf '\x00\x00\x0b\x00\x01\x01\x01\x0a\x01\x01\x0a\x01\x01\x0a\x01\x01\x0a\x01\x01\x0a\x01\x01\x0a\x01\x01\x0a\x01\x01\x0a\x01\x01\x0a\x01\x01\x0a\x01\x01\x0a\x01\x01\x0a' \
    > "$ARRAY_DIR/seed01_dense_arset"

# ARMSET k0 4095 "a" 4096 "alpha" 8388608 "abcdefgh" exercises sparse
# slices, a slice boundary, and the first super-directory block boundary.
printf '\x00\x01\x02\x00\x01\x13\x01\x0a\x01\x14\x01\x0b\x01\x19\x01\x10' \
    > "$ARRAY_DIR/seed02_sparse_boundaries"

# Stateful ring path: ARRING k0 5 a/alpha; ARRING k0 2 alphabet;
# ARSEEK k0 0; ARRING k0 8 RedisArray; ARLASTITEMS k0 3 REV.
printf '\x04\x05\x01\x00\x01\x05\x01\x0a\x01\x0b\x05\x00\x00\x01\x03\x01\x0c\x06\x00\x01\x01\x05\x00\x00\x01\x06\x01\x0d\x0b\x01\x00\x01\x04' \
    > "$ARRAY_DIR/seed03_ring_resize_seek"

# Stateful search path: ARMSET k0 0 alpha 1 alphabet 2 RedisArray;
# ARGREP k0 - + MATCH alpha NOCASE WITHVALUES LIMIT 2.
printf '\x01\x01\x02\x00\x01\x01\x01\x0b\x01\x02\x01\x0c\x01\x03\x01\x0d\x09\x00\x0e\x01\x00\x01\x01\x01\x0b\x01\x03' \
    > "$ARRAY_DIR/seed04_argrep"

# Stateful reduction path: ARSET k0 0 1 -1 7.9; AROP k0 0 2 SUM.
printf '\x01\x00\x02\x00\x01\x01\x01\x02\x01\x03\x01\x04\x0a\x00\x00\x01\x01\x01\x03' \
    > "$ARRAY_DIR/seed05_arop"

# Stateful delete/demotion path: ARSET k0 0 with 12 values; ARDELRANGE k0
# 2 10; ARSCAN k0 0 16 LIMIT 8.
printf '\x02\x00\x0b\x00\x01\x01\x01\x0a\x01\x01\x0a\x01\x01\x0a\x01\x01\x0a\x01\x01\x0a\x01\x01\x0a\x01\x01\x0a\x01\x01\x0a\x01\x01\x0a\x01\x01\x0a\x01\x01\x0a\x01\x01\x0a\x03\x00\x00\x01\x03\x01\x0a\x08\x01\x00\x01\x01\x01\x0c\x01\x06' \
    > "$ARRAY_DIR/seed06_delete_scan"

# SET k0 string followed by ARGET k0 0 exercises WRONGTYPE handling.
printf '\x01\x0d\x01\x00\x01\x0a\x07\x00\x00\x01\x01' \
    > "$ARRAY_DIR/seed07_wrongtype"

# Malformed ARGREP command shape with arbitrary arguments.
printf '\x00\x0e\x04\x07\x01\x0a\x01\x0b\x01\x0c\x01\x0d' \
    > "$ARRAY_DIR/seed08_invalid_shape"

# Stateful insert cursor path: ARINSERT k0 a alpha; ARNEXT k0;
# ARSEEK k0 4096; ARINSERT k0 alphabet; ARLASTITEMS k0 3.
printf '\x04\x04\x01\x00\x01\x0a\x01\x0b\x0c\x02\x00\x06\x00\x01\x14\x04\x00\x00\x01\x0c\x0b\x00\x00\x01\x04' \
    > "$ARRAY_DIR/seed09_insert_cursor"

# ARMSET sparse positions, delete them with ARDEL, then inspect ARCOUNT.
printf '\x02\x01\x02\x00\x01\x01\x01\x0a\x01\x14\x01\x0b\x01\x19\x01\x10\x02\x02\x00\x01\x01\x01\x14\x01\x19\x0c\x01\x00' \
    > "$ARRAY_DIR/seed10_ardel_metadata"

# Replication compression seeds. The first eight bytes select the scenario,
# compression level, corruption/split points, and independent I/O schedules.
# Remaining bytes are plaintext, except in raw-malformed mode.

# Valid empty payload and empty Zstd frame lifecycle.
printf '\x00\x00\x00\x00\x01\x00\x01\x01' \
    > "$REPLICATION_COMPRESSION_DIR/seed01_valid_empty"

# Valid plaintext, tiny input chunks, frequent frame flushes, tiny output reads.
printf '\x00\x02\x00\x00\x00\x01\x00\x00redis-replication-stream' \
    > "$REPLICATION_COMPRESSION_DIR/seed02_valid_chunked"

# Concatenated independently compressed frames split in the middle.
printf '\x04\x06\x00\x09\x03\x01\x02\x05first-second-third' \
    > "$REPLICATION_COMPRESSION_DIR/seed03_concatenated_frames"

# A valid stream truncated inside its final frame.
printf '\x01\x04\x05\x00\x01\x00\x01\x03truncated-frame' \
    > "$REPLICATION_COMPRESSION_DIR/seed04_truncated_frame"

# A bit flip in a valid stream.
printf '\x02\x07\x07\x05\x02\x01\x03\x04checksum-corruption' \
    > "$REPLICATION_COMPRESSION_DIR/seed05_corrupted_frame"

# Raw truncated Zstd magic and frame header.
printf '\x03\x00\x00\x00\x01\x00\x01\x01\x28\xb5\x2f\xfd\x00\x00' \
    > "$REPLICATION_COMPRESSION_DIR/seed06_raw_malformed"

# Valid stream followed by non-frame bytes.
printf '\x05\x03\x00\x00\x04\x01\x05\x02garbage-suffix' \
    > "$REPLICATION_COMPRESSION_DIR/seed07_trailing_garbage"

# Non-frame bytes before a valid stream.
printf '\x06\x05\x00\x00\x05\x00\x02\x06garbage-prefix' \
    > "$REPLICATION_COMPRESSION_DIR/seed08_leading_garbage"

# The same valid frame sequence twice, with a doubled plaintext oracle.
printf '\x07\x01\x00\x00\x07\x01\x07\x01duplicate-me' \
    > "$REPLICATION_COMPRESSION_DIR/seed09_duplicate_frames"

# High-entropy plaintext at the highest selected compression level.
printf '\x00\xff\x00\x00\xff\x01\xfe\x01\x00\xff\x10\xef\x20\xdf\x30\xcf\x40\xbf\x50\xaf\x60\x9f\x70\x8f' \
    > "$REPLICATION_COMPRESSION_DIR/seed10_high_entropy"

# LMOVEM/BLMOVEM seeds. These are stateful action-generator inputs:
# the first byte selects the command count and each action byte selects a list
# mutation or move grammar. Most matrix seeds first LPUSH four values into src.

# Default single-element LMOVEM.
printf '\x01\x00\x03\x00\x01\x00\x01\x01\x01\x02\x01\x03\x08\x00\x01\x00\x01' \
    > "$LIST_MOVE_DIR/seed01_lmovem_default"

# Direction, mode, and ordering matrix.
printf '\x01\x00\x03\x00\x01\x00\x01\x01\x01\x02\x01\x03\x09\x00\x01\x00\x00\x00\x02\x00' \
    > "$LIST_MOVE_DIR/seed02_left_left_count_obo"
printf '\x01\x00\x03\x00\x01\x00\x01\x01\x01\x02\x01\x03\x09\x00\x01\x00\x01\x00\x02\x01' \
    > "$LIST_MOVE_DIR/seed03_left_right_count_bulk"
printf '\x01\x00\x03\x00\x01\x00\x01\x01\x01\x02\x01\x03\x09\x00\x01\x01\x00\x01\x02\x00' \
    > "$LIST_MOVE_DIR/seed04_right_left_exactly_obo"
printf '\x01\x00\x03\x00\x01\x00\x01\x01\x01\x02\x01\x03\x09\x00\x01\x01\x01\x01\x02\x01' \
    > "$LIST_MOVE_DIR/seed05_right_right_exactly_bulk"
printf '\x01\x00\x03\x00\x01\x00\x01\x01\x01\x02\x01\x03\x09\x00\x01\x00\x00\x01\x01\x01' \
    > "$LIST_MOVE_DIR/seed06_left_left_exactly_bulk"
printf '\x01\x00\x03\x00\x01\x00\x01\x01\x01\x02\x01\x03\x09\x00\x01\x00\x01\x01\x01\x00' \
    > "$LIST_MOVE_DIR/seed07_left_right_exactly_obo"
printf '\x01\x00\x03\x00\x01\x00\x01\x01\x01\x02\x01\x03\x09\x00\x01\x01\x00\x00\x01\x01' \
    > "$LIST_MOVE_DIR/seed08_right_left_count_bulk"
printf '\x01\x00\x03\x00\x01\x00\x01\x01\x01\x02\x01\x03\x09\x00\x01\x01\x01\x00\x01\x00' \
    > "$LIST_MOVE_DIR/seed09_right_right_count_obo"

# Same-key rotation.
printf '\x01\x00\x03\x02\x01\x00\x01\x01\x01\x02\x01\x03\x0a\x02\x00\x01\x00\x01\x01' \
    > "$LIST_MOVE_DIR/seed10_same_key_rotation"

# BLMOVEM immediate success and bounded blocked-client registration.
printf '\x01\x00\x02\x00\x01\x00\x01\x01\x01\x02\x0b\x01\x00\x01\x00\x01\x00\x00\x01\x01' \
    > "$LIST_MOVE_DIR/seed11_blmovem_immediate"
printf '\x00\x0b\x01\x06\x01\x00\x01\x00\x01\x02\x01' \
    > "$LIST_MOVE_DIR/seed12_blmovem_missing_blocks"
printf '\x01\x00\x00\x00\x01\x03\x0b\x01\x00\x01\x00\x01\x00\x01\x02\x01' \
    > "$LIST_MOVE_DIR/seed13_blmovem_exactly_short_blocks"
printf '\x01\x00\x02\x02\x01\x00\x01\x01\x01\x02\x0c\x01\x02\x01\x00\x00\x00\x00\x01\x01' \
    > "$LIST_MOVE_DIR/seed14_blmovem_same_key"

# Missing and wrong-type sources/destinations.
printf '\x00\x08\x06\x01\x00\x01' \
    > "$LIST_MOVE_DIR/seed15_missing_source"
printf '\x01\x05\x04\x01\x03\x08\x04\x01\x00\x01' \
    > "$LIST_MOVE_DIR/seed16_wrong_type_source"
printf '\x02\x00\x02\x00\x01\x00\x01\x01\x01\x02\x05\x05\x01\x03\x09\x00\x05\x00\x01\x00\x01\x01' \
    > "$LIST_MOVE_DIR/seed17_wrong_type_destination"

# Two four-element pushes of 1024-byte alternating binary values cross listpack
# size boundaries before a bulk move.
printf '\x02\x01\x03\x00\x02\x03\x00\x02\x03\xff\x02\x03\x55\x02\x03\xaa\x00\x03\x00\x02\x03\x0d\x02\x03\xf2\x02\x03\x7f\x02\x03\x80\x09\x00\x01\x00\x01\x00\x05\x01' \
    > "$LIST_MOVE_DIR/seed18_large_binary_elements"

# Growth/shrink pipeline: push, pop with count, insert, trim, legacy LMOVE,
# multi-move, and state inspection.
printf '\x07\x01\x02\x03\x01\x00\x01\x01\x01\x02\x02\x01\x01\x03\x04\x01\x00\x03\x01\x03\x06\x03\x00\x01\x03\x01\x04\x03\x03\x01\x03\x01\x02\x07\x03\x00\x00\x01\x09\x00\x01\x01\x00\x00\x01\x01\x0f\x01\x00\x01\x03\x01\x02' \
    > "$LIST_MOVE_DIR/seed19_growth_shrink_pipeline"

# Malformed counts, trailers, arities, directions, and timeouts.
printf '\x03\x0d\x00\x0d\x04\x0e\x03\x0e\x05' \
    > "$LIST_MOVE_DIR/seed20_malformed_core"
printf '\x03\x0d\x01\x0d\x07\x0e\x01\x0e\x04' \
    > "$LIST_MOVE_DIR/seed21_malformed_trailers"

# Huge valid count remains bounded by the short source list.
printf '\x01\x00\x01\x00\x01\x00\x01\x01\x09\x00\x01\x00\x01\x00\x06\x01' \
    > "$LIST_MOVE_DIR/seed22_huge_count"

# Complete the direction x mode x ordering cross-product not represented by
# seeds 02-09.
printf '\x01\x00\x03\x00\x01\x00\x01\x01\x01\x02\x01\x03\x09\x00\x01\x00\x00\x00\x02\x01' \
    > "$LIST_MOVE_DIR/seed23_left_left_count_bulk"
printf '\x01\x00\x03\x00\x01\x00\x01\x01\x01\x02\x01\x03\x09\x00\x01\x00\x00\x01\x02\x00' \
    > "$LIST_MOVE_DIR/seed24_left_left_exactly_obo"
printf '\x01\x00\x03\x00\x01\x00\x01\x01\x01\x02\x01\x03\x09\x00\x01\x00\x01\x00\x02\x00' \
    > "$LIST_MOVE_DIR/seed25_left_right_count_obo"
printf '\x01\x00\x03\x00\x01\x00\x01\x01\x01\x02\x01\x03\x09\x00\x01\x00\x01\x01\x02\x01' \
    > "$LIST_MOVE_DIR/seed26_left_right_exactly_bulk"
printf '\x01\x00\x03\x00\x01\x00\x01\x01\x01\x02\x01\x03\x09\x00\x01\x01\x00\x00\x02\x00' \
    > "$LIST_MOVE_DIR/seed27_right_left_count_obo"
printf '\x01\x00\x03\x00\x01\x00\x01\x01\x01\x02\x01\x03\x09\x00\x01\x01\x00\x01\x02\x01' \
    > "$LIST_MOVE_DIR/seed28_right_left_exactly_bulk"
printf '\x01\x00\x03\x00\x01\x00\x01\x01\x01\x02\x01\x03\x09\x00\x01\x01\x01\x00\x02\x01' \
    > "$LIST_MOVE_DIR/seed29_right_right_count_bulk"
printf '\x01\x00\x03\x00\x01\x00\x01\x01\x01\x02\x01\x03\x09\x00\x01\x01\x01\x01\x02\x00' \
    > "$LIST_MOVE_DIR/seed30_right_right_exactly_obo"

# Count validation with otherwise-valid trailers. Keeping the BLMOVEM timeout
# valid ensures parsing reaches the zero, negative, and non-integer counts.
printf '\x03\x0d\x08\x0d\x09\x0e\x09\x0e\x0a' \
    > "$LIST_MOVE_DIR/seed31_malformed_move_counts"

# BLMOVEM NaN timeout, non-integer count, and invalid ordering.
printf '\x02\x0e\x08\x0e\x0b\x0e\x0c' \
    > "$LIST_MOVE_DIR/seed32_malformed_blmovem_values"

# LMOVEM EXACTLY with a short source must leave both keys unchanged.
printf '\x01\x00\x01\x00\x01\x00\x01\x01\x09\x00\x01\x00\x01\x01\x02\x01' \
    > "$LIST_MOVE_DIR/seed33_exactly_short_atomic"

# Stream command seeds. Every input starts from streams s0/s1, group g0, and
# pending entries 1-0 through 3-0, so these bytes select the follow-up grammar.
# XNACK SILENT IDS 1 1-0.
printf '\x00\x03\x00\x00\x00\x00\x00\x00\x00\x00' \
    > "$STREAM_DIR/seed01_xnack_silent"

# XNACK FATAL FORCE IDS 3 with a mix of pending and missing IDs.
printf '\x00\x03\x00\x02\x01\x00\x00\x02\x00\x01\x03' \
    > "$STREAM_DIR/seed02_xnack_fatal_force"

# XNACK FAIL with RETRYCOUNT after the IDS block.
printf '\x00\x03\x00\x01\x00\x01\x01\x00\x00\x00\x00\x00' \
    > "$STREAM_DIR/seed03_xnack_retrycount"

# XREAD with COUNT, MAXCOUNT, and MAXSIZE across two streams.
printf '\x00\x01\x01\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00' \
    > "$STREAM_DIR/seed04_xread_limits"

# XREADGROUP with COUNT, MAXCOUNT, MAXSIZE, and NOACK.
printf '\x00\x02\x01\x01\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00' \
    > "$STREAM_DIR/seed05_xreadgroup_limits"

# XCLAIM a pending entry, followed by XNACK to release it again.
printf '\x01\x04\x00\x00\x00\x00\x00\x00\x03\x00\x01\x00\x00\x00\x00\x00' \
    > "$STREAM_DIR/seed06_claim_nack_cycle"

# XACK and XDEL state transitions.
printf '\x01\x05\x00\x00\x00\x00\x06\x00\x00\x00\x00' \
    > "$STREAM_DIR/seed07_ack_delete"

# Malformed XNACK/XREADGROUP argument shape.
printf '\x00\x07\x00\x05\x01a\x01b\x01c' \
    > "$STREAM_DIR/seed08_invalid_shape"

echo "Seed files generated:"
echo "  string_commands: $(find "$STRING_DIR" -type f | wc -l | tr -d ' ')"
echo "  bitmap_commands: $(find "$BITMAP_DIR" -type f | wc -l | tr -d ' ')"
echo "  command_extensions: $(find "$COMMAND_EXT_DIR" -type f | wc -l | tr -d ' ')"
echo "  hash_templates: $(find "$HASH_DIR" -type f | wc -l | tr -d ' ')"
echo "  array_commands: $(find "$ARRAY_DIR" -type f | wc -l | tr -d ' ')"
echo "  replication_compression: $(find "$REPLICATION_COMPRESSION_DIR" -type f | wc -l | tr -d ' ')"
echo "  list_move_commands: $(find "$LIST_MOVE_DIR" -type f | wc -l | tr -d ' ')"
echo "  stream_commands: $(find "$STREAM_DIR" -type f | wc -l | tr -d ' ')"
