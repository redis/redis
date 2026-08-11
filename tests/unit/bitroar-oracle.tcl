# Exercise the same bitmap scenarios directly with legacy strings and Roaring
# bitmaps. Keeping the mode in every test name makes parity failures identify
# both the representation and the exact command assertion that failed.

start_server {tags {"bitmap" "bitmap-oracle"}} {
    foreach {mode bitmap_default expected_type} {
        legacy-string no string
        roaring yes bitmap
    } {
        r config set bitmap-default-roaring $bitmap_default

        test "bitmap oracle sparse SETBIT behavior ($mode)" {
            set bitmap bitmap:oracle:sparse{t}
            r del $bitmap

            assert_equal 0 [r setbit $bitmap 0 1]
            assert_equal 0 [r setbit $bitmap 2 1]
            assert_equal 0 [r setbit $bitmap 65536 1]
            assert_equal $expected_type [r type $bitmap]
            assert_equal 1 [r getbit $bitmap 65536]
            assert_equal 3 [r bitcount $bitmap]
            assert_equal 0 [r bitpos $bitmap 1]
            assert_equal 1 [r bitpos $bitmap 0]
            assert_equal {1 1 1} [r bitfield $bitmap \
                GET u1 0 GET u1 2 GET u1 65536]
        }

        test "bitmap oracle BITFIELD and range reads ($mode)" {
            set bitmap bitmap:oracle:bitfield{t}
            r del $bitmap

            assert_equal 0 [r setbit $bitmap 0 1]
            assert_equal 0 [r setbit $bitmap 9 1]
            assert_equal 0 [r setbit $bitmap 31 1]
            assert_equal $expected_type [r type $bitmap]
            assert_equal 3 [r bitcount $bitmap]
            assert_equal 1 [r bitcount $bitmap 4 24 bit]
            assert_equal 9 [r bitpos $bitmap 1 8 -1 bit]
            assert_equal {513 0 8} [r bitfield $bitmap \
                GET u10 0 SET u5 20 17 GET u8 16]
            assert_equal {17 1} [r bitfield_ro $bitmap GET u5 20 GET u1 31]
            assert_equal 1 [r getbit $bitmap 24]
            assert_equal 5 [r bitcount $bitmap]
        }

        test "bitmap oracle BITOP aliasing and missing sources ($mode)" {
            set a bitmap:oracle:bitop:a{t}
            set b bitmap:oracle:bitop:b{t}
            set c bitmap:oracle:bitop:c{t}
            set missing bitmap:oracle:bitop:missing{t}
            set out bitmap:oracle:bitop:out{t}
            set inverted bitmap:oracle:bitop:not{t}
            r del $a $b $c $missing $out $inverted

            assert_equal none [r type $missing]
            assert_equal 0 [r getbit $missing 65536]
            assert_equal 0 [r bitcount $missing]
            assert_equal -1 [r bitpos $missing 1]

            assert_equal 0 [r setbit $a 0 1]
            assert_equal 0 [r setbit $a 2 1]
            assert_equal 0 [r setbit $a 9 1]
            assert_equal 0 [r setbit $a 16 1]
            assert_equal 0 [r setbit $b 1 1]
            assert_equal 0 [r setbit $b 2 1]
            assert_equal 0 [r setbit $b 16 1]
            assert_equal 0 [r setbit $b 17 1]

            assert_equal 3 [r bitop xor $a $a $b]
            assert_equal $expected_type [r type $a]
            assert_equal 4 [r bitcount $a]

            assert_equal 3 [r bitop or $out $a $missing $c]
            assert_equal $expected_type [r type $out]
            assert_equal 4 [r bitcount $out]

            assert_equal 3 [r bitop not $inverted $out]
            assert_equal $expected_type [r type $inverted]
            assert_equal 20 [r bitcount $inverted]
            assert_equal 0 [r bitpos $inverted 0]
        }

        test "bitmap oracle reports missing keys and command errors ($mode)" {
            set bitmap bitmap:oracle:errors:bitmap{t}
            set missing bitmap:oracle:errors:missing{t}
            set wrongtype bitmap:oracle:errors:list{t}
            set out bitmap:oracle:errors:out{t}
            r del $bitmap $missing $wrongtype $out

            assert_equal none [r type $missing]
            assert_equal 0 [r getbit $missing 99]
            assert_equal 0 [r bitcount $missing]
            assert_equal -1 [r bitpos $missing 1]
            assert_equal {0} [r bitfield_ro $missing GET u4 12]

            assert_equal 1 [r rpush $wrongtype value]
            assert_equal list [r type $wrongtype]
            assert_error {WRONGTYPE*} {r setbit $wrongtype 0 1}
            assert_error {WRONGTYPE*} {r bitcount $wrongtype}
            assert_error {WRONGTYPE*} {r bitpos $wrongtype 1}
            assert_error {WRONGTYPE*} {r bitfield_ro $wrongtype GET u1 0}
            assert_error {WRONGTYPE*} {r bitop or $out $wrongtype $missing}
            assert_equal none [r type $out]

            assert_error {*out of range*} {r setbit $bitmap 0 2}
            assert_equal none [r type $bitmap]
            assert_error {ERR *syntax*} {r bitcount $missing 0}
        }

        test "bitmap oracle converts existing strings according to config ($mode)" {
            set bitmap bitmap:oracle:conversion{t}
            set original [binary format H* a55a]
            set changed [binary format H* 255a]
            r del $bitmap

            assert_equal OK [r set $bitmap $original]
            assert_equal string [r type $bitmap]
            assert_equal 1 [r setbit $bitmap 0 0]
            assert_equal $expected_type [r type $bitmap]
            assert_equal 0 [r getbit $bitmap 0]
            assert_equal 1 [r getbit $bitmap 2]
            assert_equal 7 [r bitcount $bitmap]
            assert_equal 2 [r bitpos $bitmap 1]
            assert_equal {37 90} [r bitfield_ro $bitmap GET u8 0 GET u8 8]

            if {$mode eq "legacy-string"} {
                assert_equal $changed [r get $bitmap]
            } else {
                assert_error {WRONGTYPE*} {r get $bitmap}
            }
        }

        test "bitmap oracle deterministic sparse seed ($mode)" {
            set bitmap bitmap:oracle:seed:sparse{t}
            r del $bitmap

            assert_equal 0 [r setbit $bitmap 1 1]
            assert_equal 0 [r setbit $bitmap 64 1]
            assert_equal 0 [r setbit $bitmap 4096 1]
            assert_equal $expected_type [r type $bitmap]
            assert_equal 1 [r getbit $bitmap 64]
            assert_equal 3 [r bitcount $bitmap]
            assert_equal 1 [r bitpos $bitmap 1]
            assert_equal {1 0 1} [r bitfield_ro $bitmap \
                GET u1 1 GET u1 2 GET u1 4096]
        }

        test "bitmap oracle deterministic DIFF and ONE alias seed ($mode)" {
            set a bitmap:oracle:seed:diff:a{t}
            set b bitmap:oracle:seed:diff:b{t}
            set out bitmap:oracle:seed:diff:out{t}
            r del $a $b $out

            assert_equal 0 [r setbit $a 0 1]
            assert_equal 0 [r setbit $a 2 1]
            assert_equal 0 [r setbit $b 2 1]
            assert_equal 0 [r setbit $b 3 1]

            assert_equal 1 [r bitop diff $a $a $b]
            assert_equal $expected_type [r type $a]
            assert_equal 1 [r bitcount $a]
            assert_equal 1 [r getbit $a 0]
            assert_equal 0 [r getbit $a 2]

            assert_equal 1 [r bitop one $out $a $b]
            assert_equal $expected_type [r type $out]
            assert_equal 3 [r bitcount $out]
            assert_equal 1 [r getbit $out 0]
            assert_equal 1 [r getbit $out 2]
            assert_equal 1 [r getbit $out 3]
        }

        test "bitmap oracle deterministic adjacent full ranges seed ($mode)" {
            set bitmap bitmap:oracle:seed:full{t}
            r del $bitmap

            assert_equal {0} [r bitfield $bitmap SET u8 0 255]
            assert_equal $expected_type [r type $bitmap]
            assert_equal 8 [r bitcount $bitmap]
            assert_equal 8 [r bitpos $bitmap 0]

            assert_equal {0} [r bitfield $bitmap SET u5 8 31]
            assert_equal 13 [r bitcount $bitmap]
            assert_equal 13 [r bitpos $bitmap 0]
        }

        test "bitmap oracle string command boundary ($mode)" {
            set bitmap bitmap:oracle:string-boundary{t}
            r del $bitmap

            assert_equal 0 [r setbit $bitmap 9 1]
            assert_equal $expected_type [r type $bitmap]
            assert_equal 1 [r getbit $bitmap 9]
            assert_equal 1 [r bitcount $bitmap]

            if {$mode eq "legacy-string"} {
                assert_equal 2 [r strlen $bitmap]
                assert_equal [binary format B* 0000000001000000] [r get $bitmap]
                assert_equal 3 [r append $bitmap Z]
                assert_equal 3 [r strlen $bitmap]
                assert_equal 3 [r setrange $bitmap 0 abc]
                assert_equal abc [r get $bitmap]
            } else {
                assert_error {WRONGTYPE*} {r strlen $bitmap}
                assert_error {WRONGTYPE*} {r get $bitmap}
                assert_error {WRONGTYPE*} {r append $bitmap Z}
                assert_error {WRONGTYPE*} {r setrange $bitmap 0 abc}
                assert_equal 1 [r getbit $bitmap 9]
                assert_equal 1 [r bitcount $bitmap]
            }
        }

        foreach {scenario offsets} {
            single-zero {0}
            byte-boundaries {0 7 8 15}
            word-boundaries {15 16 31 32 63 64 127 128}
            container-boundaries {4095 4096 65535 65536 131071}
            large-sparse {1 1000 100000 1000000}
        } {
            test "bitmap oracle $scenario offset corpus ($mode)" {
                set bitmap "bitmap:oracle:corpus:${scenario}{t}"
                r del $bitmap

                foreach offset $offsets {
                    assert_equal 0 [r setbit $bitmap $offset 1]
                    assert_equal 1 [r getbit $bitmap $offset]
                }

                set sorted [lsort -integer $offsets]
                set first [lindex $sorted 0]
                set last [lindex $sorted end]
                assert_equal $expected_type [r type $bitmap]
                assert_equal [llength $offsets] [r bitcount $bitmap]
                assert_equal $first [r bitpos $bitmap 1]
                assert_equal 1 [r getbit $bitmap $last]

                if {$mode eq "legacy-string"} {
                    assert_equal [expr {$last / 8 + 1}] [r strlen $bitmap]
                }
            }
        }

        test "bitmap oracle deterministic sparse fuzz corpus ($mode)" {
            for {set j 0} {$j < 16} {incr j} {
                set bitmap "bitmap:oracle:fuzz:$j{t}"
                r del $bitmap
                catch {unset seen}
                array set seen {}

                for {set i 0} {$i < 64} {incr i} {
                    set offset [expr {(($j + 1) * 97 + ($i * $i * 131)) % 250000}]
                    set seen($offset) 1
                    r setbit $bitmap $offset 1
                    assert_equal 1 [r getbit $bitmap $offset]
                }

                assert_equal $expected_type [r type $bitmap]
                assert_equal [array size seen] [r bitcount $bitmap]
            }
        }
    }

    r config set bitmap-default-roaring no
}
