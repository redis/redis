proc seed_string_bitmap {key bits} {
    r del $key
    r set $key ""
    foreach bit $bits {
        r setbit $key $bit 1
    }
}

proc seed_native_bitmap {key bits} {
    seed_string_bitmap $key $bits
    r bitmap convert $key
}

# Logical raw bytes of a bitmap value regardless of its representation.
proc bitmap_logical_raw {key} {
    if {![r exists $key]} {
        return ""
    }
    if {[r type $key] eq "bitmap"} {
        return [r debug bitmap-raw $key]
    }
    return [r get $key]
}

proc assert_bitmap_has_exact_bits {key bits} {
    set unique [lsort -integer -unique $bits]
    assert_equal [llength $unique] [r bitcount $key]
    foreach bit $unique {
        assert_equal 1 [r getbit $key $bit]
    }
}

proc assert_bitmap_translated_jaccard {name left_bits right_bits expected_intersection expected_union expected_ratio} {
    set left "bitmap:native:translated:jaccard:$name:left"
    set right "bitmap:native:translated:jaccard:$name:right"
    set intersection "bitmap:native:translated:jaccard:$name:intersection"
    set union "bitmap:native:translated:jaccard:$name:union"

    seed_native_bitmap $left $left_bits
    seed_native_bitmap $right $right_bits

    r bitop and $intersection $left $right
    r bitop or $union $left $right

    set actual_intersection [r bitcount $intersection]
    set actual_union [r bitcount $union]
    assert_equal $expected_intersection $actual_intersection
    assert_equal $expected_union $actual_union
    if {$actual_union == 0} {
        set actual_ratio -1
    } else {
        set actual_ratio [format %.6f [expr {double($actual_intersection) / $actual_union}]]
    }
    assert_equal $expected_ratio $actual_ratio
}

proc assert_native_bitop_matches_string {name op source_bitsets} {
    set string_dest "bitmap:native:bitop:$name:string:dest"
    set native_dest "bitmap:native:bitop:$name:native:dest"
    set string_sources {}
    set native_sources {}

    for {set i 0} {$i < [llength $source_bitsets]} {incr i} {
        set string_key "bitmap:native:bitop:$name:string:src:$i"
        set native_key "bitmap:native:bitop:$name:native:src:$i"
        seed_string_bitmap $string_key [lindex $source_bitsets $i]
        seed_native_bitmap $native_key [lindex $source_bitsets $i]
        lappend string_sources $string_key
        lappend native_sources $native_key
    }

    set string_reply [r bitop $op $string_dest {*}$string_sources]
    set native_reply [r bitop $op $native_dest {*}$native_sources]
    assert_equal $string_reply $native_reply
    assert_equal [bitmap_logical_raw $string_dest] [bitmap_logical_raw $native_dest]
    assert_equal $string_reply [string length [bitmap_logical_raw $string_dest]]
    assert_equal $native_reply [string length [bitmap_logical_raw $native_dest]]
    if {[r exists $native_dest]} {
        # At least one native source makes the destination native.
        assert_equal bitmap [r type $native_dest]
        assert_equal string [r type $string_dest]
    }
}

proc assert_native_bitop_bitset_case {name op source_bitsets expected_bits {missing_indexes {}} {alias_index -1} {dest_seed __none__}} {
    set string_dest "bitmap:native:bitop:case:$name:string:dest"
    set native_dest "bitmap:native:bitop:case:$name:native:dest"
    set string_sources {}
    set native_sources {}
    set string_source_raws {}
    set native_source_raws {}

    r config set bitmap-default-roaring no

    if {$dest_seed eq "__none__"} {
        r del $string_dest $native_dest
    } else {
        seed_string_bitmap $string_dest $dest_seed
        seed_native_bitmap $native_dest $dest_seed
    }

    for {set i 0} {$i < [llength $source_bitsets]} {incr i} {
        set string_key "bitmap:native:bitop:case:$name:string:src:$i"
        set native_key "bitmap:native:bitop:case:$name:native:src:$i"
        if {[lsearch -exact $missing_indexes $i] >= 0} {
            r del $string_key $native_key
        } else {
            seed_string_bitmap $string_key [lindex $source_bitsets $i]
            seed_native_bitmap $native_key [lindex $source_bitsets $i]
        }
        lappend string_sources $string_key
        lappend native_sources $native_key
        lappend string_source_raws [bitmap_logical_raw $string_key]
        lappend native_source_raws [bitmap_logical_raw $native_key]
    }

    if {$alias_index >= 0} {
        set string_dest [lindex $string_sources $alias_index]
        set native_dest [lindex $native_sources $alias_index]
    }

    set string_reply [r bitop $op $string_dest {*}$string_sources]
    set native_reply [r bitop $op $native_dest {*}$native_sources]
    assert_equal $string_reply $native_reply
    assert_equal [bitmap_logical_raw $string_dest] [bitmap_logical_raw $native_dest]
    assert_equal $string_reply [string length [bitmap_logical_raw $string_dest]]
    assert_equal $native_reply [string length [bitmap_logical_raw $native_dest]]
    assert_bitmap_has_exact_bits $string_dest $expected_bits
    assert_bitmap_has_exact_bits $native_dest $expected_bits
    if {[r exists $native_dest]} {
        assert_equal bitmap [r type $native_dest]
        assert_equal bitmap-roaring [r object encoding $native_dest]
    }

    for {set i 0} {$i < [llength $source_bitsets]} {incr i} {
        if {$i == $alias_index} continue
        assert_equal [lindex $string_source_raws $i] [bitmap_logical_raw [lindex $string_sources $i]]
        assert_equal [lindex $native_source_raws $i] [bitmap_logical_raw [lindex $native_sources $i]]
    }
}

proc assert_native_bitop_raws_match_string {name op source_raws native_indexes {alias_index -1}} {
    set string_dest "bitmap:native:bitop:$name:string:dest"
    set native_dest "bitmap:native:bitop:$name:native:dest"
    set string_sources {}
    set native_sources {}
    set string_source_raws {}
    set native_source_raws {}

    r config set bitmap-default-roaring no

    for {set i 0} {$i < [llength $source_raws]} {incr i} {
        set string_key "bitmap:native:bitop:$name:string:src:$i"
        set native_key "bitmap:native:bitop:$name:native:src:$i"
        r set $string_key [lindex $source_raws $i]
        r set $native_key [lindex $source_raws $i]
        if {[lsearch -exact $native_indexes $i] >= 0} {
            r bitmap convert $native_key
        }
        lappend string_sources $string_key
        lappend native_sources $native_key
        lappend string_source_raws [bitmap_logical_raw $string_key]
        lappend native_source_raws [bitmap_logical_raw $native_key]
    }

    if {$alias_index >= 0} {
        set string_dest [lindex $string_sources $alias_index]
        set native_dest [lindex $native_sources $alias_index]
    }

    set string_reply [r bitop $op $string_dest {*}$string_sources]
    set native_reply [r bitop $op $native_dest {*}$native_sources]
    assert_equal $string_reply $native_reply
    assert_equal [bitmap_logical_raw $string_dest] [bitmap_logical_raw $native_dest]
    assert_equal $string_reply [string length [bitmap_logical_raw $string_dest]]
    assert_equal $native_reply [string length [bitmap_logical_raw $native_dest]]
    if {[r exists $native_dest] && [llength $native_indexes] > 0} {
        assert_equal bitmap [r type $native_dest]
        assert_equal string [r type $string_dest]
    }

    for {set i 0} {$i < [llength $source_raws]} {incr i} {
        if {$i == $alias_index} continue
        assert_equal [lindex $string_source_raws $i] [bitmap_logical_raw [lindex $string_sources $i]]
        assert_equal [lindex $native_source_raws $i] [bitmap_logical_raw [lindex $native_sources $i]]
    }
}

proc assert_native_bitmap_command_matches_string {name raw command} {
    set string_key "bitmap:native:read-edge:$name:string"
    set native_key "bitmap:native:read-edge:$name:native"
    r set $string_key $raw
    r set $native_key $raw
    r bitmap convert $native_key

    set string_cmd [lreplace $command 1 1 $string_key]
    set native_cmd [lreplace $command 1 1 $native_key]
    assert_equal [r {*}$string_cmd] [r {*}$native_cmd]
    assert_equal bitmap [r type $native_key]
    assert_equal bitmap-roaring [r object encoding $native_key]
}

proc assert_native_bitmap_write_matches_string {name raw command} {
    set string_key "bitmap:native:write-edge:$name:string"
    set native_key "bitmap:native:write-edge:$name:native"
    r set $string_key $raw
    r set $native_key $raw
    r bitmap convert $native_key

    set string_cmd [lreplace $command 1 1 $string_key]
    set native_cmd [lreplace $command 1 1 $native_key]
    assert_equal [r {*}$string_cmd] [r {*}$native_cmd]
    assert_equal [r get $string_key] [r debug bitmap-raw $native_key]
    assert_equal bitmap [r type $native_key]
    assert_equal bitmap-roaring [r object encoding $native_key]
}

start_server {tags {"bitmap" "bitmap-native" "needs:debug" "cluster:skip"}} {
    test {native bitmap read commands preserve type encoding and bytes} {
        set raw [binary format H* 80400100080000]

        r set bitmap:native:read $raw
        r bitmap convert bitmap:native:read

        assert_equal 1 [r getbit bitmap:native:read 0]
        assert_equal 1 [r getbit bitmap:native:read 9]
        assert_equal 0 [r getbit bitmap:native:read 10]
        assert_equal 4 [r bitcount bitmap:native:read]
        assert_equal 2 [r bitcount bitmap:native:read 8 23 bit]
        assert_equal 0 [r bitpos bitmap:native:read 1]
        assert_equal 1 [r bitpos bitmap:native:read 0]
        assert_equal 9 [r bitpos bitmap:native:read 1 8 -1 bit]
        assert_equal {1 1 1} [r bitfield_ro bitmap:native:read GET u1 0 GET u1 9 GET u1 36]
        assert_error {ERR BITFIELD_RO only supports the GET subcommand} {
            r bitfield_ro bitmap:native:read SET u8 0 255
        }

        assert_equal bitmap [r type bitmap:native:read]
        assert_equal bitmap-roaring [r object encoding bitmap:native:read]
        assert_equal $raw [r debug bitmap-raw bitmap:native:read]
    }

    test {SETBIT and GETBIT round trip native bitmap offsets} {
        seed_native_bitmap bitmap:native:setbit:loop {}

        for {set offset 0} {$offset < 100} {incr offset} {
            assert_equal 0 [r setbit bitmap:native:setbit:loop $offset 1]
            assert_equal 1 [r getbit bitmap:native:setbit:loop $offset]
            assert_equal 1 [r setbit bitmap:native:setbit:loop $offset 0]
            assert_equal 0 [r getbit bitmap:native:setbit:loop $offset]
        }

        assert_equal bitmap [r type bitmap:native:setbit:loop]
        assert_equal bitmap-roaring [r object encoding bitmap:native:setbit:loop]
        assert_equal 0 [r bitcount bitmap:native:setbit:loop]
    }

    test {SETBIT updates native bitmap values and preserves trailing zero length} {
        r set bitmap:native:setbit [binary format H* 8000]
        r bitmap convert bitmap:native:setbit

        assert_equal 0 [r setbit bitmap:native:setbit 9 1]
        assert_equal bitmap [r type bitmap:native:setbit]
        assert_equal bitmap-roaring [r object encoding bitmap:native:setbit]
        assert_equal [binary format H* 8040] [r debug bitmap-raw bitmap:native:setbit]

        assert_equal 0 [r setbit bitmap:native:setbit 23 0]
        assert_equal bitmap [r type bitmap:native:setbit]
        assert_equal [binary format H* 804000] [r debug bitmap-raw bitmap:native:setbit]

        assert_equal 1 [r setbit bitmap:native:setbit 0 0]
        assert_equal bitmap [r type bitmap:native:setbit]
        assert_equal [binary format H* 004000] [r debug bitmap-raw bitmap:native:setbit]
    }

    test {native bitmap MEMORY USAGE tracks roaring container allocation updates} {
        r config set bitmap-default-roaring yes
        r del bitmap:native:memory

        assert_equal 0 [r setbit bitmap:native:memory 0 1]
        set one_container [r memory usage bitmap:native:memory]
        assert_morethan $one_container 0

        assert_equal 0 [r setbit bitmap:native:memory 65536 1]
        set two_containers [r memory usage bitmap:native:memory]
        assert_morethan $two_containers $one_container

        assert_equal 1 [r setbit bitmap:native:memory 65536 0]
        set back_to_one [r memory usage bitmap:native:memory]
        assert_lessthan $back_to_one $two_containers
        assert_equal 1 [r bitcount bitmap:native:memory]

        assert_equal 1 [r setbit bitmap:native:memory 0 0]
        set empty [r memory usage bitmap:native:memory]
        assert_lessthan $empty $back_to_one
        assert_equal 0 [r bitcount bitmap:native:memory]

        r del bitmap:native:memory:same-container
        assert_equal 0 [r setbit bitmap:native:memory:same-container 0 1]
        set sparse_container [r memory usage bitmap:native:memory:same-container]
        for {set bit 1} {$bit <= 4096} {incr bit} {
            assert_equal 0 [r setbit bitmap:native:memory:same-container $bit 1]
        }
        set dense_container [r memory usage bitmap:native:memory:same-container]
        assert_morethan $dense_container $sparse_container
        assert_equal 4097 [r bitcount bitmap:native:memory:same-container]

        r config set bitmap-default-roaring no
        r del bitmap:native:memory bitmap:native:memory:same-container
    }

    test {bitmap commands operate on legacy and native representations with default native creation disabled} {
        r config set bitmap-default-roaring no
        set raw [binary format H* 804001]
        set string_key bitmap:native:mixed-surface:string
        set native_key bitmap:native:mixed-surface:native

        r set $string_key $raw
        r set $native_key $raw
        r bitmap convert $native_key

        assert_equal string [r type $string_key]
        assert_equal bitmap [r type $native_key]
        assert_equal bitmap-roaring [r object encoding $native_key]

        assert_equal [r setbit $string_key 23 1] [r setbit $native_key 23 1]
        assert_equal [r getbit $string_key 23] [r getbit $native_key 23]
        assert_equal [r bitcount $string_key] [r bitcount $native_key]
        assert_equal [r bitcount $string_key 3 20 bit] [r bitcount $native_key 3 20 bit]
        assert_equal [r bitpos $string_key 1] [r bitpos $native_key 1]
        assert_equal [r bitpos $string_key 0 4 -1 bit] [r bitpos $native_key 0 4 -1 bit]

        set bitfield_cmd {GET u8 0 SET u5 9 17 INCRBY i6 16 -3 GET i6 16}
        assert_equal [r bitfield $string_key {*}$bitfield_cmd] [r bitfield $native_key {*}$bitfield_cmd]
        assert_equal [r bitfield_ro $string_key GET u8 0 GET u8 16] [r bitfield_ro $native_key GET u8 0 GET u8 16]
        assert_equal [r get $string_key] [r debug bitmap-raw $native_key]

        assert_native_bitop_raws_match_string mixed-surface:bitop or \
            [list [r get $string_key] [binary format H* 0f00ff]] {0}
    }

    test {GETBIT past the native bitmap logical length returns 0} {
        seed_native_bitmap bitmap:native:getbit:past {3}

        assert_equal 1 [r getbit bitmap:native:getbit:past 3]
        assert_equal 0 [r getbit bitmap:native:getbit:past 7]
        assert_equal 0 [r getbit bitmap:native:getbit:past 100]
        assert_equal 0 [r getbit bitmap:native:getbit:past 4294967295]
        assert_error {*bit offset is*out of range*} {
            r getbit bitmap:native:getbit:past 4294967296
        }
        assert_equal [binary format H* 10] [r debug bitmap-raw bitmap:native:getbit:past]
    }

    test {SETBIT keeps the proto-max-bulk-len offset limit on native bitmaps} {
        seed_native_bitmap bitmap:native:setbit:cap {0}

        assert_error {*bit offset is*out of range*} {
            r setbit bitmap:native:setbit:cap 4294967296 1
        }
        assert_equal bitmap [r type bitmap:native:setbit:cap]
        assert_equal 1 [r bitcount bitmap:native:setbit:cap]
        r del bitmap:native:setbit:cap
    }

    test {native bitmap BITCOUNT and BITPOS cover redis-roaring integration cases} {
        seed_native_bitmap bitmap:native:countpos:fib {1 2 3 5 8 13}
        assert_equal 6 [r bitcount bitmap:native:countpos:fib]
        assert_equal 1 [r bitpos bitmap:native:countpos:fib 1]
        assert_equal 0 [r bitpos bitmap:native:countpos:fib 0]

        seed_native_bitmap bitmap:native:countpos:first-one {3 4 6 10 12}
        assert_equal 3 [r bitpos bitmap:native:countpos:first-one 1]

        seed_native_bitmap bitmap:native:countpos:first-zero {0 1 2 3 4 6}
        assert_equal 5 [r bitpos bitmap:native:countpos:first-zero 0]

        seed_native_bitmap bitmap:native:countpos:empty {}
        assert_equal -1 [r bitpos bitmap:native:countpos:empty 0]
        assert_equal -1 [r bitpos bitmap:native:countpos:empty 1]

        seed_native_bitmap bitmap:native:countpos:single-zero {0}
        assert_equal 1 [r bitpos bitmap:native:countpos:single-zero 0]
    }

    test {native bitmap BITCOUNT and BITPOS match string edge ranges} {
        set raw [binary format H* ff00f0800100007f]
        set commands {
            {bitcount key}
            {bitcount key 0 -1}
            {bitcount key 1 4}
            {bitcount key -4 -2}
            {bitcount key 3 44 bit}
            {bitcount key 4 4 bit}
            {bitcount key -20 -1 bit}
            {bitpos key 1}
            {bitpos key 0}
            {bitpos key 1 1 5}
            {bitpos key 0 1 5}
            {bitpos key 1 4 39 bit}
            {bitpos key 0 4 39 bit}
            {bitpos key 0 -8 -1 bit}
        }

        set idx 0
        foreach command $commands {
            assert_native_bitmap_command_matches_string "mixed:$idx" $raw $command
            incr idx
        }

        set all_ones [binary format H* ffff]
        foreach command {
            {bitpos key 0}
            {bitpos key 0 0 1}
            {bitpos key 0 1}
            {bitpos key 0 2}
            {bitpos key 0 0 15 bit}
        } {
            assert_native_bitmap_command_matches_string "ones:$idx" $all_ones $command
            incr idx
        }
    }

    test {native bitmap BITCOUNT and BITPOS handle container edges} {
        # Bits in distinct 2^16 containers, plus dense runs, exercise the
        # container-walking BITPOS code where uint32 and uint64 arithmetic mix.
        seed_native_bitmap bitmap:native:cap-edge {0}
        r setbit bitmap:native:cap-edge 65535 1
        r setbit bitmap:native:cap-edge 65536 1
        r setbit bitmap:native:cap-edge 131071 1

        assert_equal 4 [r bitcount bitmap:native:cap-edge]
        assert_equal 65535 [r bitpos bitmap:native:cap-edge 1 1 -1 bit]
        assert_equal 65535 [r bitpos bitmap:native:cap-edge 1 8191]
        assert_equal 131071 [r bitpos bitmap:native:cap-edge 1 65537 -1 bit]
        assert_equal 1 [r bitpos bitmap:native:cap-edge 0]
        assert_equal -1 [r bitpos bitmap:native:cap-edge 0 65535 65535 bit]
        assert_equal 2 [r bitcount bitmap:native:cap-edge 65535 65536 bit]
        r del bitmap:native:cap-edge

        # A dense run crossing a container boundary: the first clear bit
        # after the run must come from the container-level scan. With an
        # explicit BIT range every bit is set, so the reply is -1; without an
        # explicit end the logical length supplies the imaginary trailing
        # zero at bit 65568.
        seed_native_bitmap bitmap:native:run-edge {}
        r bitfield bitmap:native:run-edge SET u32 65504 4294967295 SET u32 65536 4294967295
        assert_equal 65504 [r bitpos bitmap:native:run-edge 1]
        assert_equal -1 [r bitpos bitmap:native:run-edge 0 65504 -1 bit]
        assert_equal 65568 [r bitpos bitmap:native:run-edge 0 8188]
        assert_equal 64 [r bitcount bitmap:native:run-edge]
        r del bitmap:native:run-edge
    }

    test {BITFIELD writes native bitmap values through the direct write path} {
        r set bitmap:native:bitfield [binary format H* 00]
        r bitmap convert bitmap:native:bitfield

        assert_equal {0 15} [r bitfield bitmap:native:bitfield SET u4 4 15 GET u8 0]
        assert_equal bitmap [r type bitmap:native:bitfield]
        assert_equal bitmap-roaring [r object encoding bitmap:native:bitfield]
        assert_equal [binary format H* 0f] [r debug bitmap-raw bitmap:native:bitfield]
        assert_equal {15} [r bitfield_ro bitmap:native:bitfield GET u4 4]

        assert_equal {0} [r bitfield bitmap:native:bitfield SET u1 23 0]
        assert_equal bitmap [r type bitmap:native:bitfield]
        assert_equal [binary format H* 0f0000] [r debug bitmap-raw bitmap:native:bitfield]
    }

    test {BITFIELD signed INCRBY preserves native bitmap values} {
        seed_native_bitmap bitmap:native:bitfield:signed {}

        assert_equal {0 1 1} [r bitfield bitmap:native:bitfield:signed SET i5 0 -1 INCRBY i5 0 2 GET i5 0]
        assert_equal bitmap [r type bitmap:native:bitfield:signed]
        assert_equal bitmap-roaring [r object encoding bitmap:native:bitfield:signed]
        assert_equal {1} [r bitfield_ro bitmap:native:bitfield:signed GET i5 0]
    }

    test {native bitmap BITFIELD direct paths match string edge cases} {
        set raw [binary format H* 0102030400]
        set commands {
            {bitfield key GET u4 0 GET i6 9 GET u12 17}
            {bitfield_ro key GET u4 0 GET i6 9 GET u12 17}
            {bitfield key SET u5 3 17 GET u13 0 SET i6 16 -8 GET i6 16}
            {bitfield key INCRBY u8 4 7 GET u12 0}
            {bitfield key OVERFLOW SAT INCRBY i5 9 20 GET i5 9}
            {bitfield key OVERFLOW WRAP INCRBY u4 #1 20 GET u8 0}
            {bitfield key OVERFLOW FAIL SET u2 10 5 GET u2 10}
            {bitfield key SET u1 47 0 GET u1 47}
        }

        set idx 0
        foreach command $commands {
            assert_native_bitmap_write_matches_string $idx $raw $command
            incr idx
        }

        assert_native_bitmap_write_matches_string grow-after-failed-high-write \
            [binary format H* 00] \
            {bitfield key SET u1 0 1 OVERFLOW FAIL SET u2 47 5}

        assert_native_bitmap_write_matches_string grow-after-fail-only-high-write \
            [binary format H* 00] \
            {bitfield key OVERFLOW FAIL SET u2 47 5}

        set fail_key bitmap:native:bitfield:overflow-fail-string-growth
        r config set bitmap-default-roaring no
        r set $fail_key [binary format H* 00]
        assert_equal string [r type $fail_key]
        assert_equal {{}} [r bitfield $fail_key OVERFLOW FAIL SET u2 47 5]
        assert_equal string [r type $fail_key]
        assert_equal 7 [r strlen $fail_key]
        assert_equal [binary format H* 00000000000000] [r get $fail_key]

        set watched_fail_key bitmap:native:bitfield:overflow-fail-string-growth-watch
        r set $watched_fail_key [binary format H* 00]
        r watch $watched_fail_key
        assert_equal {{}} [r bitfield $watched_fail_key OVERFLOW FAIL SET u2 47 5]
        r multi
        r ping
        assert_equal {} [r exec]
        assert_equal 7 [r strlen $watched_fail_key]
    }

    test {BITFIELD uses the same write offset limit for string and native bitmaps} {
        seed_string_bitmap bitmap:string:bitfield:limit {}
        seed_native_bitmap bitmap:native:bitfield:limit {}

        foreach key {bitmap:string:bitfield:limit bitmap:native:bitfield:limit} {
            assert_error {*ERR bit offset*out of range*} {
                r bitfield $key SET u1 4294967296 1
            }
            assert_error {*ERR bit offset*out of range*} {
                r bitfield $key SET u2 4294967295 3
            }
        }
        assert_error {*ERR bit offset*out of range*} {
            r bitfield_ro bitmap:native:bitfield:limit GET u1 4294967296
        }
        assert_equal 0 [r bitcount bitmap:string:bitfield:limit]
        assert_equal 0 [r bitcount bitmap:native:bitfield:limit]
        assert_equal string [r type bitmap:string:bitfield:limit]
        assert_equal bitmap [r type bitmap:native:bitfield:limit]
        assert_equal bitmap-roaring [r object encoding bitmap:native:bitfield:limit]

        set limit 1048576
        set oldval [config_get_set proto-max-bulk-len $limit]
        set last_allowed [expr {$limit * 8 - 1}]
        assert_equal {0} [r bitfield bitmap:native:bitfield:limit SET u1 $last_allowed 1]
        assert_error {*ERR bit offset*out of range*} {
            r bitfield bitmap:native:bitfield:limit SET u2 $last_allowed 3
        }
        assert_error {*ERR bit offset*out of range*} {
            r bitfield bitmap:native:bitfield:limit INCRBY u2 $last_allowed 1
        }
        assert_equal 1 [r bitcount bitmap:native:bitfield:limit]
        r config set proto-max-bulk-len $oldval

        r del bitmap:string:bitfield:limit bitmap:native:bitfield:limit
    }

    test {translated redis-roaring int-array bit-array and clear scenarios use core bitmap commands} {
        r config set bitmap-default-roaring yes

        set int_key bitmap:native:translated:int-array
        r del $int_key
        foreach bit {1 2 3 4 5} {
            assert_equal 0 [r setbit $int_key $bit 1]
        }
        assert_equal bitmap [r type $int_key]
        assert_bitmap_has_exact_bits $int_key {1 2 3 4 5}

        foreach bit {1 3} {
            assert_equal 1 [r setbit $int_key $bit 0]
        }
        assert_bitmap_has_exact_bits $int_key {2 4 5}
        assert_equal 2 [r bitcount $int_key 4 5 bit]

        foreach bit {4 5} {
            assert_equal 1 [r setbit $int_key $bit 0]
        }
        assert_bitmap_has_exact_bits $int_key {2}

        set range_key bitmap:native:translated:range-array
        r del $range_key
        foreach bit {0 8 16} {
            assert_equal 0 [r setbit $range_key $bit 1]
        }
        assert_bitmap_has_exact_bits $range_key {0 8 16}
        assert_equal {1 1 1} [r bitfield_ro $range_key GET u1 0 GET u1 8 GET u1 16]
        assert_equal 3 [r bitcount $range_key 0 16 bit]

        set bitarray_key bitmap:native:translated:bit-array
        r del $bitarray_key
        foreach bit {1 2 4 7 11 14 17 18 21} {
            assert_equal 0 [r setbit $bitarray_key $bit 1]
        }
        assert_bitmap_has_exact_bits $bitarray_key {1 2 4 7 11 14 17 18 21}
        assert_equal {0 1 0 1} [r bitfield_ro $bitarray_key GET u1 0 GET u1 1 GET u1 24 GET u1 21]

        r config set bitmap-default-roaring no
    }

    test {translated redis-roaring range full min and max scenarios use core bitmap commands} {
        r config set bitmap-default-roaring yes

        set range_key bitmap:native:translated:setrange
        r del $range_key
        for {set bit 0} {$bit < 5} {incr bit} {
            assert_equal 0 [r setbit $range_key $bit 1]
        }
        assert_bitmap_has_exact_bits $range_key {0 1 2 3 4}
        assert_equal 0 [r bitpos $range_key 1]
        assert_equal 5 [r bitpos $range_key 0]

        set full_key bitmap:native:translated:setfull
        r set $full_key [binary format H* ff]
        r bitmap convert $full_key
        assert_equal bitmap [r type $full_key]
        assert_equal bitmap-roaring [r object encoding $full_key]
        assert_bitmap_has_exact_bits $full_key {0 1 2 3 4 5 6 7}
        assert_equal 8 [r bitpos $full_key 0]

        set minmax_key bitmap:native:translated:minmax
        seed_native_bitmap $minmax_key {}
        assert_equal 0 [r bitcount $minmax_key]
        assert_equal -1 [r bitpos $minmax_key 1]

        assert_equal 0 [r setbit $minmax_key 100 1]
        assert_bitmap_has_exact_bits $minmax_key {100}
        assert_equal 100 [r bitpos $minmax_key 1]

        assert_equal 0 [r setbit $minmax_key 0 1]
        assert_bitmap_has_exact_bits $minmax_key {0 100}
        assert_equal 0 [r bitpos $minmax_key 1]
        assert_equal 1 [r getbit $minmax_key 100]

        assert_equal 1 [r setbit $minmax_key 0 0]
        assert_equal 1 [r setbit $minmax_key 100 0]
        assert_equal 0 [r bitcount $minmax_key]
        assert_equal -1 [r bitpos $minmax_key 1]

        assert {[r memory usage $full_key] > 0}
        r config set bitmap-default-roaring no
    }

    test {translated redis-roaring contains and jaccard scenarios use bitmap algebra} {
        set a bitmap:native:translated:contains:a
        set b bitmap:native:translated:contains:b
        set c bitmap:native:translated:contains:c
        set e bitmap:native:translated:contains:empty

        seed_native_bitmap $a {1 2 3 4 5}
        seed_native_bitmap $b {2 3}
        seed_native_bitmap $c {3 4 6}
        seed_native_bitmap $e {}

        r bitop and bitmap:native:translated:contains:some $a $b
        assert_equal 2 [r bitcount bitmap:native:translated:contains:some]

        r bitop and bitmap:native:translated:contains:none $a bitmap:native:translated:contains:missing
        assert_equal 0 [r bitcount bitmap:native:translated:contains:none]

        r bitop diff bitmap:native:translated:contains:subset-miss $b $a
        assert_equal 0 [r bitcount bitmap:native:translated:contains:subset-miss]
        assert {[r bitcount $b] < [r bitcount $a]}

        r bitop diff bitmap:native:translated:contains:not-subset $c $a
        assert_bitmap_has_exact_bits bitmap:native:translated:contains:not-subset {6}

        seed_native_bitmap bitmap:native:translated:contains:eq1 {1 2 3 4 5}
        seed_native_bitmap bitmap:native:translated:contains:eq2 {1 2 3 4 5}
        r bitop xor bitmap:native:translated:contains:eq-diff \
            bitmap:native:translated:contains:eq1 bitmap:native:translated:contains:eq2
        assert_equal 0 [r bitcount bitmap:native:translated:contains:eq-diff]

        r bitop diff bitmap:native:translated:contains:empty-subset $e $a
        assert_equal 0 [r bitcount bitmap:native:translated:contains:empty-subset]

        assert_bitmap_translated_jaccard overlap {1 2 3 4 5} {3 4 5 6 7} 3 7 0.428571
        assert_bitmap_translated_jaccard subset {1 2 3} {1 2 3 4 5} 3 5 0.600000
        assert_bitmap_translated_jaccard identical {8 13 21} {8 13 21} 3 3 1.000000
        assert_bitmap_translated_jaccard one-empty {1 2 3} {} 0 3 0.000000
        assert_bitmap_translated_jaccard disjoint {1 2} {3 4} 0 4 0.000000
        assert_bitmap_translated_jaccard empty {} {} 0 0 -1
    }

    test {BITOP stores native destinations when sources include native bitmaps} {
        r set bitmap:native:bitop:a [binary format H* f000]
        r bitmap convert bitmap:native:bitop:a
        r set bitmap:native:bitop:b [binary format H* 0fff]
        r set bitmap:native:bitop:dest [binary format H* aa]
        r bitmap convert bitmap:native:bitop:dest

        assert_equal 2 [r bitop or bitmap:native:bitop:dest bitmap:native:bitop:a bitmap:native:bitop:b]
        assert_equal bitmap [r type bitmap:native:bitop:dest]
        assert_equal [binary format H* ffff] [r debug bitmap-raw bitmap:native:bitop:dest]

        assert_equal 2 [r bitop not bitmap:native:bitop:not bitmap:native:bitop:a]
        assert_equal bitmap [r type bitmap:native:bitop:not]
        assert_equal [binary format H* 0fff] [r debug bitmap-raw bitmap:native:bitop:not]
    }

    test {BITOP NOT honors proto-max-bulk-len for native bitmap sources} {
        set limit 1048576
        set oldval [config_get_set proto-max-bulk-len [expr {$limit + 1}]]
        r config set bitmap-default-roaring yes
        r del bitop:not:native:limit bitop:not:native:too-big \
            bitop:not:native:out bitop:not:native:sentinel

        assert_equal 0 [r setbit bitop:not:native:limit [expr {$limit * 8 - 1}] 1]
        assert_equal 0 [r setbit bitop:not:native:too-big [expr {($limit + 1) * 8 - 1}] 1]
        assert_equal bitmap [r type bitop:not:native:limit]
        assert_equal bitmap [r type bitop:not:native:too-big]

        r config set proto-max-bulk-len $limit
        assert_equal $limit [r bitop not bitop:not:native:out bitop:not:native:limit]
        assert_equal bitmap [r type bitop:not:native:out]
        assert_equal 1 [r getbit bitop:not:native:out [expr {$limit * 8 - 2}]]
        assert_equal 0 [r getbit bitop:not:native:out [expr {$limit * 8 - 1}]]

        r set bitop:not:native:sentinel keep
        assert_error {*string exceeds maximum allowed size (proto-max-bulk-len)*} {
            r bitop not bitop:not:native:sentinel bitop:not:native:too-big
        }
        assert_equal string [r type bitop:not:native:sentinel]
        assert_equal keep [r get bitop:not:native:sentinel]
        assert_equal bitmap [r type bitop:not:native:too-big]

        r config set bitmap-default-roaring no
        r config set proto-max-bulk-len $oldval
        r del bitop:not:native:limit bitop:not:native:too-big \
            bitop:not:native:out bitop:not:native:sentinel
    }

    test {BITOP native bitmap sources match string bitmap results for all operations} {
        set a {0 4 5 6 20}
        set b {1 5 6 21}
        set c {2 3 5 6 7 20}

        assert_native_bitop_matches_string and AND [list $a $b $c]
        assert_native_bitop_matches_string or OR [list $a $b $c]
        assert_native_bitop_matches_string xor XOR [list $a $b $c]
        assert_native_bitop_matches_string diff DIFF [list $a $b $c]
        assert_native_bitop_matches_string diff1 DIFF1 [list $a $b $c]
        assert_native_bitop_matches_string andor ANDOR [list $a $b $c]
        assert_native_bitop_matches_string one ONE [list $a $b $c]
        assert_native_bitop_matches_string not NOT [list $a]
    }

    test {BITOP current operations cover redis-roaring algebra cases} {
        set cases {
            {diff:missing-all diff {{} {}} {} {0 1}}
            {diff:basic diff {{1 2 3 4 5} {3 4 5 6 7}} {1 2}}
            {diff:multi-subtract diff {{1 2 3 4 5 6 7 8} {2 3} {5 6}} {1 4 7 8}}
            {diff:three-subtractors diff {{1 2 3 4 5 6 7 8 9 10} {1 2} {3 4} {5 6}} {7 8 9 10}}
            {diff:subset diff {{1 2 3} {1 2 3 4 5}} {}}
            {diff:disjoint diff {{1 2 3} {7 8 9}} {1 2 3}}
            {diff:missing-first diff {{} {1 2 3}} {} {0}}
            {diff:missing-subtractor diff {{1 2 3 4} {}} {1 2 3 4} {1}}
            {diff:overlap-subtractors diff {{1 2 3 4 5 6} {2 3 4} {3 4 5}} {1 6}}
            {diff:overwrite-dest diff {{5 6 7} {6}} {5 7} {} -1 {99 100}}
            {diff:large-values diff {{1000 2000 3000 4000} {2000 3000}} {1000 4000}}
            {diff:chained-equivalent diff {{1 2 3 4 5} {3 4} {1}} {2 5}}
            {diff:dest-first-source diff {{1 2 3 4 5 6} {3 4 5}} {1 2 6} {} 0}
            {diff:dest-middle-source diff {{1 2 3 4 5 6 7 8} {2 3 4} {6 7}} {1 5 8} {} 1}
            {diff:dest-last-source diff {{10 20 30 40 50} {20 30} {40}} {10 50} {} 2}
            {diff:dest-first-empty-result diff {{7 8 9} {7 8 9 10 11}} {} {} 0}

            {diff1:missing-all diff1 {{} {}} {} {0 1}}
            {diff1:basic diff1 {{3 4 5} {1 2 3 4 5 6 7}} {1 2 6 7}}
            {diff1:multi-source diff1 {{2 3 5 6} {1 2 3 4} {5 6 7 8}} {1 4 7 8}}
            {diff1:three-sources diff1 {{1 2 5 6 9 10} {1 2 3} {4 5 6} {7 8 9}} {3 4 7 8}}
            {diff1:subset diff1 {{1 2 3 4 5} {2 3 4}} {}}
            {diff1:disjoint diff1 {{1 2 3} {7 8 9}} {7 8 9}}
            {diff1:missing-first diff1 {{} {5 6 7 8}} {5 6 7 8} {0}}
            {diff1:missing-y diff1 {{1 2 3 4} {}} {} {1}}
            {diff1:all-y-missing diff1 {{10 20 30} {} {}} {} {1 2}}
            {diff1:overlap-y diff1 {{3 4 5} {1 2 3 4} {4 5 6 7}} {1 2 6 7}}
            {diff1:overwrite-dest diff1 {{5 6} {5 6 7 8}} {7 8} {} -1 {99 100}}
            {diff1:large-values diff1 {{2000 3000} {1000 2000 3000 4000}} {1000 4000}}
            {diff1:chained-equivalent diff1 {{1} {1 2 5}} {2 5}}
            {diff1:dest-x-source diff1 {{3 4 5} {1 2 3 4 5 6}} {1 2 6} {} 0}
            {diff1:dest-first-y diff1 {{2 3 4} {1 2 3 4 5 6} {6 7 8}} {1 5 6 7 8} {} 1}
            {diff1:dest-middle-y diff1 {{5 10 15} {1 5 10} {10 15 20} {15 20 25}} {1 20 25} {} 2}
            {diff1:dest-last-y diff1 {{20 30} {10 20 30} {30 40 50}} {10 40 50} {} 2}
            {diff1:dest-y-empty-result diff1 {{7 8 9 10 11} {7 8 9}} {} {} 1}
            {diff1:equal-x-y diff1 {{100 200 300} {100 200 300}} {}}
            {diff1:y-union-equals-x diff1 {{1 2 3 4 5 6} {1 2 3} {4 5 6}} {}}
            {diff1:four-y diff1 {{5 10 15 20 25 30} {1 5} {10 11} {15 16} {20 21}} {1 11 16 21}}

            {andor:basic andor {{1 2 3 4} {3 4 5 6}} {3 4}}
            {andor:three andor {{1 2 3} {2 3 4} {3 4 5}} {2 3}}
            {andor:disjoint andor {{1 2} {3 4} {5 6}} {}}
            {andor:missing-middle andor {{1 2 3} {} {2 3 4}} {2 3} {1}}
            {andor:missing-first andor {{} {1 2 3} {2 3 4}} {} {0}}
            {andor:many andor {{1 2 3 4 5} {2 3} {3 4} {4 5} {5 6} {6 7} {7 8} {8 9} {9 10} {10 11}} {2 3 4 5}}
            {andor:overwrite-dest andor {{1 2} {1}} {1} {} -1 {100 200}}
            {andor:dest-first-source andor {{1 2 3 10 20} {2 3 4 10 30} {3 4 5 10 40}} {2 3 10} {} 0}

            {one:single one {{1 3 5}} {1 3 5}}
            {one:non-overlap one {{1 3 5} {2 4 6}} {1 2 3 4 5 6}}
            {one:overlap one {{1 2 3} {3 4 5}} {1 2 4 5}}
            {one:three one {{0 4 5 6} {1 5 6} {2 3 5 6 7}} {0 1 2 3 4 7}}
            {one:all-same one {{10 20 30} {10 20 30} {10 20 30}} {}}
            {one:missing-middle one {{1 2 3} {} {3 4 5}} {1 2 4 5} {1}}
            {one:complex-overlap one {{1 2 3 4 5} {2 3 4 6 7} {3 4 5 7 8} {4 5 6 8 9}} {1 9}}
            {one:large-values one {{1000000 2000000} {2000000 3000000}} {1000000 3000000}}
            {one:overwrite-dest one {{1 2} {2 3}} {1 3} {} -1 {100 200 300}}
        }

        foreach case $cases {
            set missing_indexes {}
            set alias_index -1
            set dest_seed __none__
            lassign $case name op sources expected missing_indexes alias_index dest_seed
            assert_native_bitop_bitset_case $name $op $sources $expected $missing_indexes $alias_index $dest_seed
        }
    }

    test {BITOP current operation syntax errors are preserved on native paths} {
        seed_native_bitmap bitmap:native:bitop:syntax:a {1}
        seed_native_bitmap bitmap:native:bitop:syntax:b {2}

        assert_error {ERR syntax error} {
            r bitop noop bitmap:native:bitop:syntax:dest bitmap:native:bitop:syntax:a bitmap:native:bitop:syntax:b
        }
        assert_error {ERR BITOP NOT*} {
            r bitop not bitmap:native:bitop:syntax:dest bitmap:native:bitop:syntax:a bitmap:native:bitop:syntax:b
        }
        assert_error {ERR BITOP DIFF*} {
            r bitop diff bitmap:native:bitop:syntax:dest bitmap:native:bitop:syntax:a
        }
        assert_error {ERR BITOP DIFF1*} {
            r bitop diff1 bitmap:native:bitop:syntax:dest bitmap:native:bitop:syntax:a
        }
        assert_error {ERR BITOP ANDOR*} {
            r bitop andor bitmap:native:bitop:syntax:dest bitmap:native:bitop:syntax:a
        }
    }

    test {BITOP handles native bitmap empty sources and destination aliasing} {
        seed_native_bitmap bitmap:native:bitop:empty {}
        assert_equal 0 [r bitop not bitmap:native:bitop:empty-not bitmap:native:bitop:empty]
        assert_equal 0 [r exists bitmap:native:bitop:empty-not]

        seed_string_bitmap bitmap:native:bitop:alias:string:dest {0 2 4 6}
        seed_string_bitmap bitmap:native:bitop:alias:string:other {2 6 8}
        seed_native_bitmap bitmap:native:bitop:alias:native:dest {0 2 4 6}
        seed_native_bitmap bitmap:native:bitop:alias:native:other {2 6 8}

        set string_reply [r bitop diff bitmap:native:bitop:alias:string:dest bitmap:native:bitop:alias:string:dest bitmap:native:bitop:alias:string:other]
        set native_reply [r bitop diff bitmap:native:bitop:alias:native:dest bitmap:native:bitop:alias:native:dest bitmap:native:bitop:alias:native:other]
        assert_equal $string_reply $native_reply
        assert_equal [r get bitmap:native:bitop:alias:string:dest] [r debug bitmap-raw bitmap:native:bitop:alias:native:dest]
        assert_equal bitmap [r type bitmap:native:bitop:alias:native:dest]
    }

    test {BITOP mixed native and string sources match string results for all operations} {
        set a [binary format H* f000ff]
        set b [binary format H* 0f0f]
        set c [binary format H* 33000080]
        set raws [list $a $b $c]

        foreach op {and or xor diff diff1 andor one} {
            assert_native_bitop_raws_match_string "mixed:$op" $op $raws {0 2}
        }
        assert_native_bitop_raws_match_string mixed:not not [list $a] {0}
    }

    test {BITOP mixed native source destination aliasing matches string results} {
        set a [binary format H* aa5500]
        set b [binary format H* 0ff0]
        set c [binary format H* 330000f0]
        set raws [list $a $b $c]

        foreach {op alias_index native_indexes} {
            and   0 {0 2}
            or    1 {1 2}
            xor   2 {0 2}
            diff  0 {0 2}
            diff1 1 {1 2}
            andor 2 {0 2}
            one   0 {0 2}
        } {
            assert_native_bitop_raws_match_string "alias:$op:$alias_index" \
                $op $raws $native_indexes $alias_index
        }

        foreach {op alias_index native_indexes} {
            and   0 {2}
            or    1 {0 2}
            xor   2 {0}
            diff  0 {2}
            diff1 1 {0 2}
            andor 2 {0}
            one   0 {2}
        } {
            assert_native_bitop_raws_match_string "alias-string:$op:$alias_index" \
                $op $raws $native_indexes $alias_index
        }

        assert_native_bitop_raws_match_string alias:not not [list $a] {0} 0
    }

    test {BITOP mixed native fuzz matches bitmap-default-roaring no strings} {
        foreach op {and or xor diff diff1 andor one} {
            set min_args 1
            if {$op eq "diff" || $op eq "diff1" || $op eq "andor"} {
                set min_args 2
            }

            for {set i 0} {$i < 12} {incr i} {
                set raws {}
                set native_indexes {}
                set count [expr {$min_args + [randomInt 4]}]

                for {set j 0} {$j < $count} {incr j} {
                    lappend raws [randstring 0 128]
                    if {[expr {($i + $j) % 2}] == 0} {
                        lappend native_indexes $j
                    }
                }

                assert_native_bitop_raws_match_string "fuzz:$op:$i" \
                    $op $raws $native_indexes
            }
        }

        for {set i 0} {$i < 12} {incr i} {
            assert_native_bitop_raws_match_string "fuzz:not:$i" \
                not [list [randstring 0 128]] {0}
        }
    }

    test {BITOP mixed native and missing-key sources match string results} {
        r config set bitmap-default-roaring no

        set a [binary format H* f0f0]
        set c [binary format H* 0f]

        foreach op {and or xor diff diff1 andor one} {
            r del bitop:miss:string:dest bitop:miss:native:dest
            r del bitop:miss:string:a bitop:miss:string:gone bitop:miss:string:c
            r del bitop:miss:native:a bitop:miss:native:gone bitop:miss:native:c

            r set bitop:miss:string:a $a
            r set bitop:miss:string:c $c
            r set bitop:miss:native:a $a
            r set bitop:miss:native:c $c
            r bitmap convert bitop:miss:native:a

            set string_reply [r bitop $op bitop:miss:string:dest \
                bitop:miss:string:a bitop:miss:string:gone bitop:miss:string:c]
            set native_reply [r bitop $op bitop:miss:native:dest \
                bitop:miss:native:a bitop:miss:native:gone bitop:miss:native:c]
            assert_equal $string_reply $native_reply
            assert_equal [bitmap_logical_raw bitop:miss:string:dest] \
                [bitmap_logical_raw bitop:miss:native:dest]
        }
    }

    test {BITOP with a missing first source matches string results on the native path} {
        # The empty-accumulator seeding branches (sources[0] == NULL) are
        # distinct code paths: AND/ANDOR clear the result, DIFF1 skips the
        # andnot, and the generic copy falls back to an empty roaring.
        r config set bitmap-default-roaring no

        set a [binary format H* f0f0]
        set c [binary format H* 0f]

        foreach op {and or xor diff diff1 andor one} {
            r del bitop:first:string:dest bitop:first:native:dest
            r del bitop:first:string:gone bitop:first:string:a bitop:first:string:c
            r del bitop:first:native:gone bitop:first:native:a bitop:first:native:c

            r set bitop:first:string:a $a
            r set bitop:first:string:c $c
            r set bitop:first:native:a $a
            r set bitop:first:native:c $c
            r bitmap convert bitop:first:native:a

            set string_reply [r bitop $op bitop:first:string:dest \
                bitop:first:string:gone bitop:first:string:a bitop:first:string:c]
            set native_reply [r bitop $op bitop:first:native:dest \
                bitop:first:native:gone bitop:first:native:a bitop:first:native:c]
            assert_equal $string_reply $native_reply
            assert_equal [bitmap_logical_raw bitop:first:string:dest] \
                [bitmap_logical_raw bitop:first:native:dest]
        }
    }

    test {BITOP duplicate sources match string results on the native path} {
        r config set bitmap-default-roaring no

        set a [binary format H* aa5500]
        set s [binary format H* 0ff0]

        # The same native bitmap key twice: both slots borrow the same
        # roaring, so the accumulator must deep-copy rather than steal.
        foreach op {and or xor diff diff1 andor one} {
            r del bitop:dup:string:dest bitop:dup:native:dest
            r del bitop:dup:string:k bitop:dup:native:k
            r set bitop:dup:string:k $a
            r set bitop:dup:native:k $a
            r bitmap convert bitop:dup:native:k

            set string_reply [r bitop $op bitop:dup:string:dest \
                bitop:dup:string:k bitop:dup:string:k]
            set native_reply [r bitop $op bitop:dup:native:dest \
                bitop:dup:native:k bitop:dup:native:k]
            assert_equal $string_reply $native_reply
            assert_equal [bitmap_logical_raw bitop:dup:string:dest] \
                [bitmap_logical_raw bitop:dup:native:dest]
        }

        # The same string key twice alongside a native source: each slot
        # builds an independent owned roaring, so the slot-0 steal cannot
        # affect the second operand.
        foreach op {and or xor diff diff1 andor one} {
            r del bitop:dup2:string:dest bitop:dup2:native:dest
            r del bitop:dup2:string:s bitop:dup2:native:s
            r del bitop:dup2:string:n bitop:dup2:native:n
            r set bitop:dup2:string:s $s
            r set bitop:dup2:native:s $s
            r set bitop:dup2:string:n $a
            r set bitop:dup2:native:n $a
            r bitmap convert bitop:dup2:native:n

            set string_reply [r bitop $op bitop:dup2:string:dest \
                bitop:dup2:string:s bitop:dup2:string:s bitop:dup2:string:n]
            set native_reply [r bitop $op bitop:dup2:native:dest \
                bitop:dup2:native:s bitop:dup2:native:s bitop:dup2:native:n]
            assert_equal $string_reply $native_reply
            assert_equal [bitmap_logical_raw bitop:dup2:string:dest] \
                [bitmap_logical_raw bitop:dup2:native:dest]
        }
    }

    test {BITOP rejects non-string non-bitmap sources mixed with native bitmaps} {
        seed_native_bitmap bitop:wrongtype:native {0 9}
        r del bitop:wrongtype:list bitop:wrongtype:dest
        r rpush bitop:wrongtype:list element

        # The type error fires after earlier sources may already be prepared,
        # exercising the cleanup of converted operands under sanitizer runs.
        assert_error {WRONGTYPE*} {
            r bitop and bitop:wrongtype:dest bitop:wrongtype:native bitop:wrongtype:list
        }
        assert_error {WRONGTYPE*} {
            r bitop xor bitop:wrongtype:dest bitop:wrongtype:list bitop:wrongtype:native
        }
        assert_equal 0 [r exists bitop:wrongtype:dest]
        assert_equal bitmap [r type bitop:wrongtype:native]
        assert_equal bitmap-roaring [r object encoding bitop:wrongtype:native]
    }
}
