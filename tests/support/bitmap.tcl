# Shared helpers for tests that need a Roaring bitmap fixture without exposing
# a user-facing conversion command.

# Re-encodes an existing plain-string bitmap as Roaring in place, exercising the
# string->Roaring conversion path while preserving the original bytes. It sets bit
# 0 back to its current value (a no-op write that triggers conversion without
# changing content) with bitmap-default-roaring enabled, then restores the setting.
# The zero-length case is handled via RESTORE of an empty Roaring payload, since any
# SETBIT there would extend the value. Fixtures built from scratch use direct SETBIT.
proc convert_string_bitmap_to_roaring {client key} {
    set old [lindex [$client config get bitmap-default-roaring] 1]
    set raw [$client get $key]

    if {[string length $raw] == 0} {
        set ttl [$client pttl $key]
        if {$ttl < 0} {
            set ttl 0
        }
        $client restore $key $ttl [empty_roaring_bitmap_dump_payload] replace
        return OK
    }

    $client config set bitmap-default-roaring yes
    set code [catch {
        binary scan [string index $raw 0] cu first_byte
        $client setbit $key 0 [expr {($first_byte & 0x80) != 0}]
    } result opts]
    $client config set bitmap-default-roaring $old
    if {$code != 0} {
        return -options $opts $result
    }
    return OK
}

proc empty_roaring_bitmap_dump_payload {} {
    # RDB_TYPE_BITMAP, zero logical byte length, an eight-byte portable
    # Roaring payload with zero high-32 buckets, RDB_VERSION 16, followed by
    # an all-zero checksum. RESTORE accepts the zero checksum in test-built
    # payloads.
    return [binary format H* 210008000000000000000010000000000000000000]
}
