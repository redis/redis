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
ARRAY_DIR="$CORPUS_DIR/array_commands"
STREAM_DIR="$CORPUS_DIR/stream_commands"

mkdir -p "$STRING_DIR" "$BITMAP_DIR" "$ARRAY_DIR" "$STREAM_DIR"
rm -f "$STRING_DIR"/seed* "$BITMAP_DIR"/seed* "$ARRAY_DIR"/seed* "$STREAM_DIR"/seed*

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
echo "  array_commands: $(find "$ARRAY_DIR" -type f | wc -l | tr -d ' ')"
echo "  stream_commands: $(find "$STREAM_DIR" -type f | wc -l | tr -d ' ')"
