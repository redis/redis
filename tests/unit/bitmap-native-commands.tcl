proc seed_string_bitmap {key bits} {
    r del $key
    r set $key ""
    foreach bit $bits {
        r setbit $key $bit 1
    }
}

proc seed_native_bitmap {key bits} {
    seed_string_bitmap $key $bits
    r debug bitmap-force-roaring $key
}

proc bitmap_raw_or_empty {key} {
    if {[r exists $key]} {
        return [r get $key]
    }
    return ""
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
    assert_equal [bitmap_raw_or_empty $string_dest] [bitmap_raw_or_empty $native_dest]
    if {[r exists $native_dest]} {
        assert_equal string [r type $native_dest]
    }
}

proc assert_native_bitmap_command_matches_string {name raw command} {
    set string_key "bitmap:native:read-edge:$name:string"
    set native_key "bitmap:native:read-edge:$name:native"
    r set $string_key $raw
    r set $native_key $raw
    r debug bitmap-force-roaring $native_key

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
    r debug bitmap-force-roaring $native_key

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
        r debug bitmap-force-roaring bitmap:native:read

        assert_equal 1 [r getbit bitmap:native:read 0]
        assert_equal 1 [r getbit bitmap:native:read 9]
        assert_equal 0 [r getbit bitmap:native:read 10]
        assert_equal 4 [r bitcount bitmap:native:read]
        assert_equal 2 [r bitcount bitmap:native:read 8 23 bit]
        assert_equal 0 [r bitpos bitmap:native:read 1]
        assert_equal 1 [r bitpos bitmap:native:read 0]
        assert_equal 9 [r bitpos bitmap:native:read 1 8 -1 bit]
        assert_equal {1 1 1} [r bitfield_ro bitmap:native:read GET u1 0 GET u1 9 GET u1 36]

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
        r debug bitmap-force-roaring bitmap:native:setbit

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

    test {BITFIELD writes native bitmap values through materialization fallback} {
        r set bitmap:native:bitfield [binary format H* 00]
        r debug bitmap-force-roaring bitmap:native:bitfield

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
    }

    test {BITFIELD rejects native bitmap writes past the roaring offset range} {
        seed_native_bitmap bitmap:native:bitfield:limit {}

        assert_error {*ERR bit offset is not representable in native bitmap encoding*} {
            r bitfield bitmap:native:bitfield:limit SET u2 4294967295 3
        }
        assert_equal bitmap [r type bitmap:native:bitfield:limit]
        assert_equal bitmap-roaring [r object encoding bitmap:native:bitfield:limit]
        assert_equal "" [r debug bitmap-raw bitmap:native:bitfield:limit]
    }

    test {BITOP accepts native bitmap sources and stores string destinations} {
        r set bitmap:native:bitop:a [binary format H* f000]
        r debug bitmap-force-roaring bitmap:native:bitop:a
        r set bitmap:native:bitop:b [binary format H* 0fff]
        r set bitmap:native:bitop:dest [binary format H* aa]
        r debug bitmap-force-roaring bitmap:native:bitop:dest

        assert_equal 2 [r bitop or bitmap:native:bitop:dest bitmap:native:bitop:a bitmap:native:bitop:b]
        assert_equal string [r type bitmap:native:bitop:dest]
        assert_equal [binary format H* ffff] [r get bitmap:native:bitop:dest]

        assert_equal 2 [r bitop not bitmap:native:bitop:not bitmap:native:bitop:a]
        assert_equal string [r type bitmap:native:bitop:not]
        assert_equal [binary format H* 0fff] [r get bitmap:native:bitop:not]
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
        assert_equal [r get bitmap:native:bitop:alias:string:dest] [r get bitmap:native:bitop:alias:native:dest]
        assert_equal string [r type bitmap:native:bitop:alias:native:dest]
    }
}
