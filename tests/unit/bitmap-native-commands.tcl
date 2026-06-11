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

proc assert_native_bitop_raws_match_string {name op source_raws native_indexes {alias_index -1}} {
    set string_dest "bitmap:native:bitop:$name:string:dest"
    set native_dest "bitmap:native:bitop:$name:native:dest"
    set string_sources {}
    set native_sources {}

    r config set bitmap-roaring-enabled no
    r config set bitmap-roaring-auto-convert no

    for {set i 0} {$i < [llength $source_raws]} {incr i} {
        set string_key "bitmap:native:bitop:$name:string:src:$i"
        set native_key "bitmap:native:bitop:$name:native:src:$i"
        r set $string_key [lindex $source_raws $i]
        r set $native_key [lindex $source_raws $i]
        if {[lsearch -exact $native_indexes $i] >= 0} {
            r debug bitmap-force-roaring $native_key
        }
        lappend string_sources $string_key
        lappend native_sources $native_key
    }

    if {$alias_index >= 0} {
        set string_dest [lindex $string_sources $alias_index]
        set native_dest [lindex $native_sources $alias_index]
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

    test {GETBIT past the native bitmap logical length returns 0} {
        seed_native_bitmap bitmap:native:getbit:past {3}

        assert_equal 1 [r getbit bitmap:native:getbit:past 3]
        assert_equal 0 [r getbit bitmap:native:getbit:past 7]
        assert_equal 0 [r getbit bitmap:native:getbit:past 100]
        assert_equal 0 [r getbit bitmap:native:getbit:past 4294967295]
        assert_equal [binary format H* 10] [r debug bitmap-raw bitmap:native:getbit:past]
    }

    test {SETBIT enforces the native bitmap offset cap} {
        seed_native_bitmap bitmap:native:setbit:cap {0}

        # The last representable bit must be accepted: it extends the logical
        # length to exactly the 512MB cap without materializing anything.
        assert_equal 0 [r setbit bitmap:native:setbit:cap 4294967295 1]
        assert_equal 1 [r getbit bitmap:native:setbit:cap 4294967295]
        assert_equal 2 [r bitcount bitmap:native:setbit:cap]

        # One past the cap is rejected with the native error even when a
        # raised proto-max-bulk-len lets the generic offset check pass, and
        # the key is left untouched.
        set old_proto [config_get_set proto-max-bulk-len 1073741824]
        set e [catch {
            r setbit bitmap:native:setbit:cap 4294967296 1
        } err]
        r config set proto-max-bulk-len $old_proto
        assert {$e == 1}
        assert_match {*ERR bit offset is not representable in native bitmap encoding*} $err
        assert_equal bitmap [r type bitmap:native:setbit:cap]
        assert_equal bitmap-roaring [r object encoding bitmap:native:setbit:cap]
        assert_equal 2 [r bitcount bitmap:native:setbit:cap]
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

    test {native bitmap BITCOUNT and BITPOS handle the 2^32-1 cap edge} {
        # The cap edge is exactly where the iterator-based BITPOS code mixes
        # uint32 and uint64 arithmetic; setting the last representable bit on
        # an already-native key is cheap (no 512MB materialization as long as
        # the assertions stick to native BITCOUNT/BITPOS/GETBIT paths).
        seed_native_bitmap bitmap:native:cap-edge {0}
        r setbit bitmap:native:cap-edge 4294967295 1

        assert_equal 2 [r bitcount bitmap:native:cap-edge]
        assert_equal 4294967295 [r bitpos bitmap:native:cap-edge 1 1 -1 bit]
        assert_equal 4294967295 [r bitpos bitmap:native:cap-edge 1 536870911]
        assert_equal 1 [r bitpos bitmap:native:cap-edge 0]
        assert_equal -1 [r bitpos bitmap:native:cap-edge 0 4294967295 4294967295 bit]
        assert_equal 4294967288 [r bitpos bitmap:native:cap-edge 0 -1 -1]
        r del bitmap:native:cap-edge
    }

    test {BITFIELD writes native bitmap values through the direct write path} {
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

        assert_native_bitmap_write_matches_string grow-after-fail-only-high-write \
            [binary format H* 00] \
            {bitfield key OVERFLOW FAIL SET u2 47 5}
    }

    test {BITFIELD rejects native bitmap writes past the roaring offset range} {
        seed_native_bitmap bitmap:native:bitfield:limit {}

        assert_error {*ERR bit offset is not representable in native bitmap encoding*} {
            r bitfield bitmap:native:bitfield:limit SET u2 4294967295 3
        }
        set old_proto [config_get_set proto-max-bulk-len 1073741824]
        set e [catch {
            r bitfield bitmap:native:bitfield:limit SET u1 4294967296 1
        } err]
        r config set proto-max-bulk-len $old_proto
        assert {$e == 1}
        assert_match {*ERR bit offset is not representable in native bitmap encoding*} $err
        assert_equal bitmap [r type bitmap:native:bitfield:limit]
        assert_equal bitmap-roaring [r object encoding bitmap:native:bitfield:limit]
        assert_equal "" [r debug bitmap-raw bitmap:native:bitfield:limit]

        # The acceptance side of the same boundary: a write whose last bit is
        # exactly 2^32-1 must succeed. Avoid bitmap-raw afterwards - the key
        # now has a 512MB logical length.
        assert_equal {0} [r bitfield bitmap:native:bitfield:limit SET u1 4294967295 1]
        assert_equal 1 [r getbit bitmap:native:bitfield:limit 4294967295]
        assert_equal 1 [r bitcount bitmap:native:bitfield:limit]
        assert_equal bitmap [r type bitmap:native:bitfield:limit]
        assert_equal bitmap-roaring [r object encoding bitmap:native:bitfield:limit]
        r del bitmap:native:bitfield:limit
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

        assert_native_bitop_raws_match_string alias:not not [list $a] {0} 0
    }

    test {BITOP mixed native fuzz matches native-conversion-disabled strings} {
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
        r config set bitmap-roaring-enabled no
        r config set bitmap-roaring-auto-convert no

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
            r debug bitmap-force-roaring bitop:miss:native:a

            set string_reply [r bitop $op bitop:miss:string:dest \
                bitop:miss:string:a bitop:miss:string:gone bitop:miss:string:c]
            set native_reply [r bitop $op bitop:miss:native:dest \
                bitop:miss:native:a bitop:miss:native:gone bitop:miss:native:c]
            assert_equal $string_reply $native_reply
            assert_equal [bitmap_raw_or_empty bitop:miss:string:dest] \
                [bitmap_raw_or_empty bitop:miss:native:dest]
        }
    }

    test {BITOP with a missing first source matches string results on the native path} {
        # The empty-accumulator seeding branches (sources[0] == NULL) are
        # distinct code paths: AND/ANDOR clear the result, DIFF1 skips the
        # andnot, and the generic copy falls back to an empty roaring.
        r config set bitmap-roaring-enabled no
        r config set bitmap-roaring-auto-convert no

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
            r debug bitmap-force-roaring bitop:first:native:a

            set string_reply [r bitop $op bitop:first:string:dest \
                bitop:first:string:gone bitop:first:string:a bitop:first:string:c]
            set native_reply [r bitop $op bitop:first:native:dest \
                bitop:first:native:gone bitop:first:native:a bitop:first:native:c]
            assert_equal $string_reply $native_reply
            assert_equal [bitmap_raw_or_empty bitop:first:string:dest] \
                [bitmap_raw_or_empty bitop:first:native:dest]
        }
    }

    test {BITOP duplicate sources match string results on the native path} {
        r config set bitmap-roaring-enabled no
        r config set bitmap-roaring-auto-convert no

        set a [binary format H* aa5500]
        set s [binary format H* 0ff0]

        # The same native bitmap key twice: both slots borrow the same
        # roaring, so the accumulator must deep-copy rather than steal.
        foreach op {and or xor diff diff1 andor one} {
            r del bitop:dup:string:dest bitop:dup:native:dest
            r del bitop:dup:string:k bitop:dup:native:k
            r set bitop:dup:string:k $a
            r set bitop:dup:native:k $a
            r debug bitmap-force-roaring bitop:dup:native:k

            set string_reply [r bitop $op bitop:dup:string:dest \
                bitop:dup:string:k bitop:dup:string:k]
            set native_reply [r bitop $op bitop:dup:native:dest \
                bitop:dup:native:k bitop:dup:native:k]
            assert_equal $string_reply $native_reply
            assert_equal [bitmap_raw_or_empty bitop:dup:string:dest] \
                [bitmap_raw_or_empty bitop:dup:native:dest]
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
            r debug bitmap-force-roaring bitop:dup2:native:n

            set string_reply [r bitop $op bitop:dup2:string:dest \
                bitop:dup2:string:s bitop:dup2:string:s bitop:dup2:string:n]
            set native_reply [r bitop $op bitop:dup2:native:dest \
                bitop:dup2:native:s bitop:dup2:native:s bitop:dup2:native:n]
            assert_equal $string_reply $native_reply
            assert_equal [bitmap_raw_or_empty bitop:dup2:string:dest] \
                [bitmap_raw_or_empty bitop:dup2:native:dest]
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
