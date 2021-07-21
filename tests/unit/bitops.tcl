# Compare Redis commands against Tcl implementations of the same commands.
proc count_bits s {
    binary scan $s b* bits
    string length [regsub -all {0} $bits {}]
}

proc simulate_bit_op {op args} {
    set maxlen 0
    set j 0
    set count [llength $args]
    foreach a $args {
        binary scan $a b* bits
        set b($j) $bits
        if {[string length $bits] > $maxlen} {
            set maxlen [string length $bits]
        }
        incr j
    }
    for {set j 0} {$j < $count} {incr j} {
        if {[string length $b($j)] < $maxlen} {
            append b($j) [string repeat 0 [expr $maxlen-[string length $b($j)]]]
        }
    }
    set out {}
    for {set x 0} {$x < $maxlen} {incr x} {
        set bit [string range $b(0) $x $x]
        if {$op eq {not}} {set bit [expr {!$bit}]}
        for {set j 1} {$j < $count} {incr j} {
            set bit2 [string range $b($j) $x $x]
            switch $op {
                and {set bit [expr {$bit & $bit2}]}
                or  {set bit [expr {$bit | $bit2}]}
                xor {set bit [expr {$bit ^ $bit2}]}
            }
        }
        append out $bit
    }
    binary format b* $out
}

start_server {tags {"bitops"}} {
    test {BITCOUNT returns 0 against non existing key} {
        r bitcount no-key
    } 0

    test {BITCOUNT returns 0 with out of range indexes} {
        r set str "xxxx"
        r bitcount str 4 10
    } 0

    test {BITCOUNT returns 0 with negative indexes where start > end} {
        r set str "xxxx"
        r bitcount str -6 -7
    } 0

    catch {unset num}
    foreach vec [list "" "\xaa" "\x00\x00\xff" "foobar" "123"] {
        incr num
        test "BITCOUNT against test vector #$num" {
            r set str $vec
            assert {[r bitcount str] == [count_bits $vec]}
        }
    }

    test {BITCOUNT fuzzing without start/end} {
        for {set j 0} {$j < 100} {incr j} {
            set str [randstring 0 3000]
            r set str $str
            assert {[r bitcount str] == [count_bits $str]}
        }
    }

    test {BITCOUNT fuzzing with start/end} {
        for {set j 0} {$j < 100} {incr j} {
            set str [randstring 0 3000]
            r set str $str
            set l [string length $str]
            set start [randomInt $l]
            set end [randomInt $l]
            if {$start > $end} {
                lassign [list $end $start] start end
            }
            assert {[r bitcount str $start $end] == [count_bits [string range $str $start $end]]}
        }
    }

    test {BITCOUNT with start, end} {
        r set s "foobar"
        assert_equal [r bitcount s 0 -1] [count_bits "foobar"]
        assert_equal [r bitcount s 1 -2] [count_bits "ooba"]
        assert_equal [r bitcount s -2 1] [count_bits ""]
        assert_equal [r bitcount s 0 1000] [count_bits "foobar"]
    }

    test {BITCOUNT syntax error #1} {
        catch {r bitcount s 0} e
        set e
    } {ERR*syntax*}

    test {BITCOUNT regression test for github issue #582} {
        r del foo
        r setbit foo 0 1
        if {[catch {r bitcount foo 0 4294967296} e]} {
            assert_match {*ERR*out of range*} $e
            set _ 1
        } else {
            set e
        }
    } {1}

    test {BITCOUNT misaligned prefix} {
        r del str
        r set str ab
        r bitcount str 1 -1
    } {3}

    test {BITCOUNT misaligned prefix + full words + remainder} {
        r del str
        r set str __PPxxxxxxxxxxxxxxxxRR__
        r bitcount str 2 -3
    } {74}

    test {BITOP NOT (empty string)} {
        r set s ""
        r bitop not dest s
        r get dest
    } {}

    test {BITOP NOT (known string)} {
        r set s "\xaa\x00\xff\x55"
        r bitop not dest s
        r get dest
    } "\x55\xff\x00\xaa"

    test {BITOP where dest and target are the same key} {
        r set s "\xaa\x00\xff\x55"
        r bitop not s s
        r get s
    } "\x55\xff\x00\xaa"

    test {BITOP AND|OR|XOR don't change the string with single input key} {
        r set a "\x01\x02\xff"
        r bitop and res1 a
        r bitop or  res2 a
        r bitop xor res3 a
        list [r get res1] [r get res2] [r get res3]
    } [list "\x01\x02\xff" "\x01\x02\xff" "\x01\x02\xff"]

    test {BITOP missing key is considered a stream of zero} {
        r set a "\x01\x02\xff"
        r bitop and res1 no-suck-key a
        r bitop or  res2 no-suck-key a no-such-key
        r bitop xor res3 no-such-key a
        list [r get res1] [r get res2] [r get res3]
    } [list "\x00\x00\x00" "\x01\x02\xff" "\x01\x02\xff"]

    test {BITOP shorter keys are zero-padded to the key with max length} {
        r set a "\x01\x02\xff\xff"
        r set b "\x01\x02\xff"
        r bitop and res1 a b
        r bitop or  res2 a b
        r bitop xor res3 a b
        list [r get res1] [r get res2] [r get res3]
    } [list "\x01\x02\xff\x00" "\x01\x02\xff\xff" "\x00\x00\x00\xff"]

    foreach op {and or xor} {
        test "BITOP $op fuzzing" {
            for {set i 0} {$i < 10} {incr i} {
                r flushall
                set vec {}
                set veckeys {}
                set numvec [expr {[randomInt 10]+1}]
                for {set j 0} {$j < $numvec} {incr j} {
                    set str [randstring 0 1000]
                    lappend vec $str
                    lappend veckeys vector_$j
                    r set vector_$j $str
                }
                r bitop $op target {*}$veckeys
                assert_equal [r get target] [simulate_bit_op $op {*}$vec]
            }
        }
    }

    test {BITOP NOT fuzzing} {
        for {set i 0} {$i < 10} {incr i} {
            r flushall
            set str [randstring 0 1000]
            r set str $str
            r bitop not target str
            assert_equal [r get target] [simulate_bit_op not $str]
        }
    }

    test {BITOP with integer encoded source objects} {
        r set a 1
        r set b 2
        r bitop xor dest a b a
        r get dest
    } {2}

    test {BITOP with non string source key} {
        r del c
        r set a 1
        r set b 2
        r lpush c foo
        catch {r bitop xor dest a b c d} e
        set e
    } {WRONGTYPE*}

    test {BITOP with empty string after non empty string (issue #529)} {
        r flushdb
        r set a "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        r bitop or x a b
    } {32}

    test {BITPOS bit=0 with empty key returns 0} {
        r del str
        r bitpos str 0
    } {0}

    test {BITPOS bit=1 with empty key returns -1} {
        r del str
        r bitpos str 1
    } {-1}

    test {BITPOS bit=0 with string less than 1 word works} {
        r set str "\xff\xf0\x00"
        r bitpos str 0
    } {12}

    test {BITPOS bit=1 with string less than 1 word works} {
        r set str "\x00\x0f\x00"
        r bitpos str 1
    } {12}

    test {BITPOS bit=0 starting at unaligned address} {
        r set str "\xff\xf0\x00"
        r bitpos str 0 1
    } {12}

    test {BITPOS bit=1 starting at unaligned address} {
        r set str "\x00\x0f\xff"
        r bitpos str 1 1
    } {12}

    test {BITPOS bit=0 unaligned+full word+reminder} {
        r del str
        r set str "\xff\xff\xff" ; # Prefix
        # Followed by two (or four in 32 bit systems) full words
        r append str "\xff\xff\xff\xff\xff\xff\xff\xff"
        r append str "\xff\xff\xff\xff\xff\xff\xff\xff"
        r append str "\xff\xff\xff\xff\xff\xff\xff\xff"
        # First zero bit.
        r append str "\x0f"
        assert {[r bitpos str 0] == 216}
        assert {[r bitpos str 0 1] == 216}
        assert {[r bitpos str 0 2] == 216}
        assert {[r bitpos str 0 3] == 216}
        assert {[r bitpos str 0 4] == 216}
        assert {[r bitpos str 0 5] == 216}
        assert {[r bitpos str 0 6] == 216}
        assert {[r bitpos str 0 7] == 216}
        assert {[r bitpos str 0 8] == 216}
    }

    test {BITPOS bit=1 unaligned+full word+reminder} {
        r del str
        r set str "\x00\x00\x00" ; # Prefix
        # Followed by two (or four in 32 bit systems) full words
        r append str "\x00\x00\x00\x00\x00\x00\x00\x00"
        r append str "\x00\x00\x00\x00\x00\x00\x00\x00"
        r append str "\x00\x00\x00\x00\x00\x00\x00\x00"
        # First zero bit.
        r append str "\xf0"
        assert {[r bitpos str 1] == 216}
        assert {[r bitpos str 1 1] == 216}
        assert {[r bitpos str 1 2] == 216}
        assert {[r bitpos str 1 3] == 216}
        assert {[r bitpos str 1 4] == 216}
        assert {[r bitpos str 1 5] == 216}
        assert {[r bitpos str 1 6] == 216}
        assert {[r bitpos str 1 7] == 216}
        assert {[r bitpos str 1 8] == 216}
    }

    test {BITPOS bit=1 returns -1 if string is all 0 bits} {
        r set str ""
        for {set j 0} {$j < 20} {incr j} {
            assert {[r bitpos str 1] == -1}
            r append str "\x00"
        }
    }

    test {BITPOS bit=0 works with intervals} {
        r set str "\x00\xff\x00"
        assert {[r bitpos str 0 0 -1] == 0}
        assert {[r bitpos str 0 1 -1] == 16}
        assert {[r bitpos str 0 2 -1] == 16}
        assert {[r bitpos str 0 2 200] == 16}
        assert {[r bitpos str 0 1 1] == -1}
    }

    test {BITPOS bit=1 works with intervals} {
        r set str "\x00\xff\x00"
        assert {[r bitpos str 1 0 -1] == 8}
        assert {[r bitpos str 1 1 -1] == 8}
        assert {[r bitpos str 1 2 -1] == -1}
        assert {[r bitpos str 1 2 200] == -1}
        assert {[r bitpos str 1 1 1] == 8}
    }

    test {BITPOS bit=0 changes behavior if end is given} {
        r set str "\xff\xff\xff"
        assert {[r bitpos str 0] == 24}
        assert {[r bitpos str 0 0] == 24}
        assert {[r bitpos str 0 0 -1] == -1}
    }

    test {BITPOS bit=1 fuzzy testing using SETBIT} {
        r del str
        set max 524288; # 64k
        set first_one_pos -1
        for {set j 0} {$j < 1000} {incr j} {
            assert {[r bitpos str 1] == $first_one_pos}
            set pos [randomInt $max]
            r setbit str $pos 1
            if {$first_one_pos == -1 || $first_one_pos > $pos} {
                # Update the position of the first 1 bit in the array
                # if the bit we set is on the left of the previous one.
                set first_one_pos $pos
            }
        }
    }

    test {BITPOS bit=0 fuzzy testing using SETBIT} {
        set max 524288; # 64k
        set first_zero_pos $max
        r set str [string repeat "\xff" [expr $max/8]]
        for {set j 0} {$j < 1000} {incr j} {
            assert {[r bitpos str 0] == $first_zero_pos}
            set pos [randomInt $max]
            r setbit str $pos 0
            if {$first_zero_pos > $pos} {
                # Update the position of the first 0 bit in the array
                # if the bit we clear is on the left of the previous one.
                set first_zero_pos $pos
            }
        }
    }
}

start_server {tags {"bitops large-memory"}} {
    test "BIT pos larger than UINT_MAX" {
        set bytes [expr (1 << 29) + 1]
        set bitpos [expr (1 << 32)]
        set oldval [lindex [r config get proto-max-bulk-len] 1]
        r config set proto-max-bulk-len $bytes
        r setbit mykey $bitpos 1
        assert_equal $bytes [r strlen mykey]
        assert_equal 1 [r getbit mykey $bitpos]
        assert_equal [list 128 128 -1] [r bitfield mykey get u8 $bitpos set u8 $bitpos 255 get i8 $bitpos]
        assert_equal $bitpos [r bitpos mykey 1]
        assert_equal $bitpos [r bitpos mykey 1 [expr $bytes - 1]]
        if {$::accurate} {
            # set all bits to 1
            set mega [expr (1 << 23)]
            set part [string repeat "\xFF" $mega]
            for {set i 0} {$i < 64} {incr i} {
                r setrange mykey [expr $i * $mega] $part
            }
            r setrange mykey [expr $bytes - 1] "\xFF"
            assert_equal [expr $bitpos + 8] [r bitcount mykey]
            assert_equal -1 [r bitpos mykey 0 0 [expr $bytes - 1]]
        }
        r config set proto-max-bulk-len $oldval
        r del mykey
    } {1}
}
