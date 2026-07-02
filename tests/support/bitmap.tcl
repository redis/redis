# Helpers for tests that need a native bitmap fixture without exposing a
# user-facing conversion command. They create or convert through ordinary
# bitmap writes while bitmap-default-roaring is enabled, then restore the
# previous server setting.

proc convert_string_bitmap_to_native {client key} {
    set old [lindex [$client config get bitmap-default-roaring] 1]
    set raw [$client get $key]

    $client config set bitmap-default-roaring yes
    set code [catch {
        if {[string length $raw] == 0} {
            $client setbit $key 0 0
        } else {
            binary scan [string index $raw 0] cu first_byte
            $client setbit $key 0 [expr {($first_byte & 0x80) != 0}]
        }
    } result opts]
    $client config set bitmap-default-roaring $old
    if {$code != 0} {
        return -options $opts $result
    }
    return OK
}

proc create_native_bitmap_from_raw {client key raw} {
    $client set $key $raw
    convert_string_bitmap_to_native $client $key
}

proc create_native_bitmap_from_bits {client key bits} {
    set old [lindex [$client config get bitmap-default-roaring] 1]

    $client del $key
    $client config set bitmap-default-roaring yes
    set code [catch {
        if {[llength $bits] == 0} {
            $client setbit $key 0 0
        } else {
            foreach bit $bits {
                $client setbit $key $bit 1
            }
        }
    } result opts]
    $client config set bitmap-default-roaring $old
    if {$code != 0} {
        return -options $opts $result
    }
    return OK
}

proc seed_native_bitmap_raw {key raw} {
    create_native_bitmap_from_raw r $key $raw
}

proc seed_native_bitmap {key bits} {
    create_native_bitmap_from_bits r $key $bits
}
