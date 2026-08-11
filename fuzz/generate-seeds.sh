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
COMMAND_EXT_DIR="$CORPUS_DIR/command_extensions"
STREAM_DIR="$CORPUS_DIR/stream_commands"

mkdir -p "$STRING_DIR" "$BITMAP_DIR" "$COMMAND_EXT_DIR" "$STREAM_DIR"
rm -f "$STRING_DIR"/seed* "$BITMAP_DIR"/seed* "$COMMAND_EXT_DIR"/seed* "$STREAM_DIR"/seed*

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
echo "  stream_commands: $(find "$STREAM_DIR" -type f | wc -l | tr -d ' ')"
