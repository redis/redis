# Shared helpers for tests that need a Roaring bitmap fixture without exposing
# a user-facing conversion command.

# Re-encodes an existing string bitmap as Roaring without changing its logical
# value or TTL. BITCONVERT is an internal replay primitive, so temporarily mark
# the test connection internal and always restore its ordinary-client state.
# Callers must pass a connection that is not already marked internal.
proc convert_string_bitmap_to_roaring {client key} {
    set code [catch {
        $client debug mark-internal-client
        $client bitconvert $key ROARING
    } result opts]
    set cleanup_code [catch {
        $client debug mark-internal-client unmark
    } cleanup_result cleanup_opts]
    if {$code != 0} {
        return -options $opts $result
    }
    if {$cleanup_code != 0} {
        return -options $cleanup_opts $cleanup_result
    }
    return $result
}

proc empty_roaring_bitmap_dump_payload {} {
    # RDB_TYPE_BITMAP, zero logical byte length, an eight-byte portable
    # Roaring payload with zero high-32 buckets, RDB_VERSION 16, followed by
    # an all-zero checksum. RESTORE accepts the zero checksum in test-built
    # payloads.
    return [binary format H* 210008000000000000000010000000000000000000]
}
