#!/usr/bin/env bash
#
# Generate initial seed corpora for the Redis core fuzz targets.
#
# These are generator inputs, not RESP payloads. The first byte selects the
# command count, each command-kind byte selects a grammar branch, and following
# bytes choose keys, offsets, values, arities, and modes.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CORPUS_DIR="$SCRIPT_DIR/corpus"
STRING_DIR="$CORPUS_DIR/string_commands"
BITMAP_DIR="$CORPUS_DIR/bitmap_commands"
REPLICATION_COMPRESSION_DIR="$CORPUS_DIR/replication_compression"
LIST_MOVE_DIR="$CORPUS_DIR/list_move_commands"
STREAM_DIR="$CORPUS_DIR/stream_commands"

mkdir -p "$STRING_DIR" "$BITMAP_DIR" "$REPLICATION_COMPRESSION_DIR" "$LIST_MOVE_DIR" "$STREAM_DIR"
rm -f "$STRING_DIR"/seed* "$BITMAP_DIR"/seed* \
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
echo "  replication_compression: $(find "$REPLICATION_COMPRESSION_DIR" -type f | wc -l | tr -d ' ')"
echo "  list_move_commands: $(find "$LIST_MOVE_DIR" -type f | wc -l | tr -d ' ')"
echo "  stream_commands: $(find "$STREAM_DIR" -type f | wc -l | tr -d ' ')"
