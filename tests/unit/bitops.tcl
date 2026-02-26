# Compare Redis commands against Tcl implementations of the same commands.
proc count_bits s {
    binary scan $s b* bits
    string length [regsub -all {0} $bits {}]
}

# start end are bit index
proc count_bits_start_end {s start end} {
    binary scan $s B* bits
    string length [regsub -all {0} [string range $bits $start $end] {}]
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
        set fst_bit [string range $b(0) $x $x]
        set bit $fst_bit
        if {[expr {$op == {diff} || $op == {diff1} || $op == {andor}}]} {
            set bit "0"
        }
        if {$op eq {not}} {set bit [expr {!$bit}]}
        set multi_cnt "0"
        for {set j 1} {$j < $count} {incr j} {
            set bit2 [string range $b($j) $x $x]
            switch $op {
                and   {set bit [expr {$bit & $bit2}]}
                or    {set bit [expr {$bit | $bit2}]}
                xor   {set bit [expr {$bit ^ $bit2}]}
                diff  {set bit [expr {$bit | $bit2}]}
                diff1 {set bit [expr {$bit | $bit2}]}
                andor {set bit [expr {$bit | $bit2}]}
                one {
                    set multi_cnt [expr $multi_cnt | {$bit & $bit2}]
                    set bit [expr {$bit ^ $bit2}]
                    set bit [expr {$bit & !$multi_cnt}]
                }
            }
        }
        switch $op {
            diff  { set bit [expr {$fst_bit & !$bit}] }
            diff1 { set bit [expr {!$fst_bit & $bit}] }
            andor { set bit [expr {$fst_bit & $bit}] }
        }

        append out $bit
    }
    binary format b* $out
}

start_server {tags {"bitops"}} {
    test {BITCOUNT against wrong type} {
        r del mylist
        r lpush mylist a b c
        assert_error "*WRONGTYPE*" {r bitcount mylist}
        assert_error "*WRONGTYPE*" {r bitcount mylist 0 100}

        # with negative indexes where start > end
        assert_error "*WRONGTYPE*" {r bitcount mylist -6 -7}
        assert_error "*WRONGTYPE*" {r bitcount mylist -6 -15 bit}
    }

    test {BITCOUNT returns 0 against non existing key} {
        r del no-key
        assert {[r bitcount no-key] == 0}
        assert {[r bitcount no-key 0 1000 bit] == 0}
    }

    test {BITCOUNT returns 0 with out of range indexes} {
        r set str "xxxx"
        assert {[r bitcount str 4 10] == 0}
        assert {[r bitcount str 32 87 bit] == 0}
    }

    test {BITCOUNT returns 0 with negative indexes where start > end} {
        r set str "xxxx"
        assert {[r bitcount str -6 -7] == 0}
        assert {[r bitcount str -6 -15 bit] == 0}

        # against non existing key
        r del str
        assert {[r bitcount str -6 -7] == 0}
        assert {[r bitcount str -6 -15 bit] == 0}
    }

    catch {unset num}
    foreach vec [list "" "\xaa" "\x00\x00\xff" "foobar" "123"] {
        incr num
        test "BITCOUNT against test vector #$num" {
            r set str $vec
            set count [count_bits $vec]
            assert {[r bitcount str] == $count}
            assert {[r bitcount str 0 -1 bit] == $count}
        }
    }

    test {BITCOUNT fuzzing without start/end} {
        for {set j 0} {$j < 100} {incr j} {
            set str [randstring 0 3000]
            r set str $str
            set count [count_bits $str]
            assert {[r bitcount str] == $count}
            assert {[r bitcount str 0 -1 bit] == $count}
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
                # Swap start and end
                lassign [list $end $start] start end
            }
            assert {[r bitcount str $start $end] == [count_bits [string range $str $start $end]]}
        }

        for {set j 0} {$j < 100} {incr j} {
            set str [randstring 0 3000]
            r set str $str
            set l [expr [string length $str] * 8]
            set start [randomInt $l]
            set end [randomInt $l]
            if {$start > $end} {
                # Swap start and end
                lassign [list $end $start] start end
            }
            assert {[r bitcount str $start $end bit] == [count_bits_start_end $str $start $end]}
        }
    }

    test {BITCOUNT with start, end} {
        set s "foobar"
        r set s $s
        assert_equal [r bitcount s 0 -1] [count_bits "foobar"]
        assert_equal [r bitcount s 1 -2] [count_bits "ooba"]
        assert_equal [r bitcount s -2 1] [count_bits ""]
        assert_equal [r bitcount s 0 1000] [count_bits "foobar"]

        assert_equal [r bitcount s 0 -1 bit] [count_bits $s]
        assert_equal [r bitcount s 10 14 bit] [count_bits_start_end $s 10 14]
        assert_equal [r bitcount s 3 14 bit] [count_bits_start_end $s 3 14]
        assert_equal [r bitcount s 3 29 bit] [count_bits_start_end $s 3 29]
        assert_equal [r bitcount s 10 -34 bit] [count_bits_start_end $s 10 14]
        assert_equal [r bitcount s 3 -34 bit] [count_bits_start_end $s 3 14]
        assert_equal [r bitcount s 3 -19 bit] [count_bits_start_end $s 3 29]
        assert_equal [r bitcount s -2 1 bit] 0
        assert_equal [r bitcount s 0 1000 bit] [count_bits $s]
    }

    test {BITCOUNT with illegal arguments} {
        # Used to return 0 for non-existing key instead of errors
        r del s
        assert_error {ERR *syntax*} {r bitcount s 0}
        assert_error {ERR *syntax*} {r bitcount s 0 1 hello}
        assert_error {ERR *syntax*} {r bitcount s 0 1 hello hello2}

        r set s 1
        assert_error {ERR *syntax*} {r bitcount s 0}
        assert_error {ERR *syntax*} {r bitcount s 0 1 hello}
        assert_error {ERR *syntax*} {r bitcount s 0 1 hello hello2}
    }

    test {BITCOUNT against non-integer value} {
        # against existing key
        r set s 1
        assert_error {ERR *not an integer*} {r bitcount s a b}

        # against non existing key
        r del s
        assert_error {ERR *not an integer*} {r bitcount s a b}

        # against wrong type
        r lpush s a b c
        assert_error {ERR *not an integer*} {r bitcount s a b}
    }

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
        r set s{t} ""
        r bitop not dest{t} s{t}
        r get dest{t}
    } {}

    test {BITOP NOT (known string)} {
        r set s{t} "\xaa\x00\xff\x55"
        r bitop not dest{t} s{t}
        r get dest{t}
    } "\x55\xff\x00\xaa"

    test {BITOP NOT with multiple source keys} {
        r set s{t} "\xaa\x00\xff\x55"
        assert_error "ERR BITOP NOT*" { r bitop not dest{t} s{t} s{t} }
    }

    test {BITOP where dest and target are the same key} {
        r set s "\xaa\x00\xff\x55"
        r bitop not s s
        r get s
    } "\x55\xff\x00\xaa"

    test {BITOP AND|OR|XOR|ONE don't change the string with single input key} {
        r set a{t} "\x01\x02\xff"
        r bitop and res1{t} a{t}
        r bitop or  res2{t} a{t}
        r bitop xor res3{t} a{t}
        r bitop one res4{t} a{t}
        list [r get res1{t}] [r get res2{t}] [r get res3{t}] [r get res4{t}]
    } [list "\x01\x02\xff" "\x01\x02\xff" "\x01\x02\xff" "\x01\x02\xff"]

    test {BITOP DIFF|DIFF1|ANDOR with one source key} {
        r set s{t} ""
        assert_error "ERR BITOP DIFF*" { r bitop diff dest{t} s{t} }
        assert_error "ERR BITOP DIFF1*" { r bitop diff1 dest{t} s{t} }
        assert_error "ERR BITOP ANDOR*" { r bitop andor dest{t} s{t} }
    }

    test {BITOP missing key is considered a stream of zero} {
        r set a{t} "\x01\x02\xff"
        r bitop and   res1{t} no-such-key{t} a{t}
        r bitop or    res2{t} no-such-key{t} a{t} no-such-key{t}
        r bitop xor   res3{t} no-such-key{t} a{t}
        r bitop diff  res4{t} a{t} no-such-key{t}
        r bitop diff1 res5{t} a{t} no-such-key{t}
        r bitop andor res6{t} a{t} no-such-key{t}
        r bitop one   res7{t} no-such_key{t} a{t}
        list [r get res1{t}] [r get res2{t}] [r get res3{t}] [r get res4{t}] [r get res5{t}] [r get res6{t}] [r get res7{t}]
    } [list "\x00\x00\x00" "\x01\x02\xff" "\x01\x02\xff" "\x01\x02\xff" "\x00\x00\x00" "\x00\x00\x00" "\x01\x02\xff"]

    test {BITOP shorter keys are zero-padded to the key with max length} {
        r set a{t} "\x01\x02\xff\xff"
        r set b{t} "\x01\x02\xff"
        r bitop and   res1{t} a{t} b{t}
        r bitop or    res2{t} a{t} b{t}
        r bitop xor   res3{t} a{t} b{t}
        r bitop diff  res4{t} a{t} b{t}
        r bitop diff1 res5{t} a{t} b{t}
        r bitop andor res6{t} a{t} b{t}
        r bitop one   res7{t} a{t} b{t}
        list [r get res1{t}] [r get res2{t}] [r get res3{t}] [r get res4{t}] [r get res5{t}] [r get res6{t}] [r get res7{t}]
    } [list "\x01\x02\xff\x00" "\x01\x02\xff\xff" "\x00\x00\x00\xff" "\x00\x00\x00\xff" "\x00\x00\x00\x00" "\x01\x02\xff\x00" "\x00\x00\x00\xff"]

    foreach op {and or xor diff diff1 andor one} {
        test "BITOP $op fuzzing" {
            set min_args 1
            if {[expr {$op == {diff} || $op == {diff1} || $op == {andor}}]} {
                set min_args 2
            }
            for {set i 0} {$i < 10} {incr i} {
                r flushall
                set vec {}
                set veckeys {}
                set numvec [expr {[randomInt 10]+$min_args}]
                for {set j 0} {$j < $numvec} {incr j} {
                    set str [randstring 0 1000]
                    lappend vec $str
                    lappend veckeys vector_$j{t}
                    r set vector_$j{t} $str
                }
                r bitop $op target{t} {*}$veckeys
                assert_equal [r get target{t}] [simulate_bit_op $op {*}$vec]
            }
        }
    }

    test {BITOP NOT fuzzing} {
        for {set i 0} {$i < 10} {incr i} {
            r flushall
            set str [randstring 0 1000]
            r set str{t} $str
            r bitop not target{t} str{t}
            assert_equal [r get target{t}] [simulate_bit_op not $str]
        }
    }

    test {BITOP with integer encoded source objects} {
        r set a{t} 1
        r set b{t} 2
        r bitop xor dest{t} a{t} b{t} a{t}
        r get dest{t}
    } {2}

    test {BITOP with non string source key} {
        r del c{t}
        r set a{t} 1
        r set b{t} 2
        r lpush c{t} foo
        catch {r bitop xor dest{t} a{t} b{t} c{t} d{t}} e
        set e
    } {WRONGTYPE*}

    test {BITOP with empty string after non empty string (issue #529)} {
        r flushdb
        r set a{t} "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        r bitop or x{t} a{t} b{t}
    } {32}

    test {BITPOS against wrong type} {
        r del mylist
        r lpush mylist a b c
        assert_error "*WRONGTYPE*" {r bitpos mylist 0}
        assert_error "*WRONGTYPE*" {r bitpos mylist 1 10 100}
    }

    test {BITPOS will illegal arguments} {
        # Used to return 0 for non-existing key instead of errors
        r del s
        assert_error {ERR *syntax*} {r bitpos s 0 1 hello hello2}
        assert_error {ERR *syntax*} {r bitpos s 0 0 1 hello}

        r set s 1
        assert_error {ERR *syntax*} {r bitpos s 0 1 hello hello2}
        assert_error {ERR *syntax*} {r bitpos s 0 0 1 hello}
    }

    test {BITPOS against non-integer value} {
        # against existing key
        r set s 1
        assert_error {ERR *not an integer*} {r bitpos s a}
        assert_error {ERR *not an integer*} {r bitpos s 0 a b}

        # against non existing key
        r del s
        assert_error {ERR *not an integer*} {r bitpos s b}
        assert_error {ERR *not an integer*} {r bitpos s 0 a b}

        # against wrong type
        r lpush s a b c
        assert_error {ERR *not an integer*} {r bitpos s a}
        assert_error {ERR *not an integer*} {r bitpos s 1 a b}
    }

    test {BITPOS bit=0 with empty key returns 0} {
        r del str
        assert {[r bitpos str 0] == 0}
        assert {[r bitpos str 0 0 -1 bit] == 0}
    }

    test {BITPOS bit=1 with empty key returns -1} {
        r del str
        assert {[r bitpos str 1] == -1}
        assert {[r bitpos str 1 0 -1] == -1}
    }

    test {BITPOS bit=0 with string less than 1 word works} {
        r set str "\xff\xf0\x00"
        assert {[r bitpos str 0] == 12}
        assert {[r bitpos str 0 0 -1 bit] == 12}
    }

    test {BITPOS bit=1 with string less than 1 word works} {
        r set str "\x00\x0f\x00"
        assert {[r bitpos str 1] == 12}
        assert {[r bitpos str 1 0 -1 bit] == 12}
    }

    test {BITPOS bit=0 starting at unaligned address} {
        r set str "\xff\xf0\x00"
        assert {[r bitpos str 0 1] == 12}
        assert {[r bitpos str 0 1 -1 bit] == 12}
    }

    test {BITPOS bit=1 starting at unaligned address} {
        r set str "\x00\x0f\xff"
        assert {[r bitpos str 1 1] == 12}
        assert {[r bitpos str 1 1 -1 bit] == 12}
    }

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

        assert {[r bitpos str 0 1 -1 bit] == 216}
        assert {[r bitpos str 0 9 -1 bit] == 216}
        assert {[r bitpos str 0 17 -1 bit] == 216}
        assert {[r bitpos str 0 25 -1 bit] == 216}
        assert {[r bitpos str 0 33 -1 bit] == 216}
        assert {[r bitpos str 0 41 -1 bit] == 216}
        assert {[r bitpos str 0 49 -1 bit] == 216}
        assert {[r bitpos str 0 57 -1 bit] == 216}
        assert {[r bitpos str 0 65 -1 bit] == 216}
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

        assert {[r bitpos str 1 1 -1 bit] == 216}
        assert {[r bitpos str 1 9 -1 bit] == 216}
        assert {[r bitpos str 1 17 -1 bit] == 216}
        assert {[r bitpos str 1 25 -1 bit] == 216}
        assert {[r bitpos str 1 33 -1 bit] == 216}
        assert {[r bitpos str 1 41 -1 bit] == 216}
        assert {[r bitpos str 1 49 -1 bit] == 216}
        assert {[r bitpos str 1 57 -1 bit] == 216}
        assert {[r bitpos str 1 65 -1 bit] == 216}
    }

    test {BITPOS bit=1 returns -1 if string is all 0 bits} {
        r set str ""
        for {set j 0} {$j < 20} {incr j} {
            assert {[r bitpos str 1] == -1}
            assert {[r bitpos str 1 0 -1 bit] == -1}
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

        assert {[r bitpos str 0 0 -1 bit] == 0}
        assert {[r bitpos str 0 8 -1 bit] == 16}
        assert {[r bitpos str 0 16 -1 bit] == 16}
        assert {[r bitpos str 0 16 200 bit] == 16}
        assert {[r bitpos str 0 8 8 bit] == -1}
    }

    test {BITPOS bit=1 works with intervals} {
        r set str "\x00\xff\x00"
        assert {[r bitpos str 1 0 -1] == 8}
        assert {[r bitpos str 1 1 -1] == 8}
        assert {[r bitpos str 1 2 -1] == -1}
        assert {[r bitpos str 1 2 200] == -1}
        assert {[r bitpos str 1 1 1] == 8}

        assert {[r bitpos str 1 0 -1 bit] == 8}
        assert {[r bitpos str 1 8 -1 bit] == 8}
        assert {[r bitpos str 1 16 -1 bit] == -1}
        assert {[r bitpos str 1 16 200 bit] == -1}
        assert {[r bitpos str 1 8 8 bit] == 8}
    }

    test {BITPOS bit=0 changes behavior if end is given} {
        r set str "\xff\xff\xff"
        assert {[r bitpos str 0] == 24}
        assert {[r bitpos str 0 0] == 24}
        assert {[r bitpos str 0 0 -1] == -1}
        assert {[r bitpos str 0 0 -1 bit] == -1}
    }

    test {SETBIT/BITFIELD only increase dirty when the value changed} {
        r del foo{t} foo2{t} foo3{t}
        set dirty [s rdb_changes_since_last_save]

        # Create a new key, always increase the dirty.
        r setbit foo{t} 0 0
        r bitfield foo2{t} set i5 0 0
        set dirty2 [s rdb_changes_since_last_save]
        assert {$dirty2 == $dirty + 2}

        # No change.
        r setbit foo{t} 0 0
        r bitfield foo2{t} set i5 0 0
        set dirty3 [s rdb_changes_since_last_save]
        assert {$dirty3 == $dirty2}

        # Do a change and a no change.
        r setbit foo{t} 0 1
        r setbit foo{t} 0 1
        r setbit foo{t} 0 0
        r setbit foo{t} 0 0
        r bitfield foo2{t} set i5 0 1
        r bitfield foo2{t} set i5 0 1
        r bitfield foo2{t} set i5 0 0
        r bitfield foo2{t} set i5 0 0
        set dirty4 [s rdb_changes_since_last_save]
        assert {$dirty4 == $dirty3 + 4}

        # BITFIELD INCRBY always increase dirty.
        r bitfield foo3{t} incrby i5 0 1
        r bitfield foo3{t} incrby i5 0 1
        set dirty5 [s rdb_changes_since_last_save]
        assert {$dirty5 == $dirty4 + 2}

        # Change length only
        r setbit foo{t} 90 0
        r bitfield foo2{t} set i5 90 0
        set dirty6 [s rdb_changes_since_last_save]
        assert {$dirty6 == $dirty5 + 2}
    }

    test {BITPOS bit=1 fuzzy testing using SETBIT} {
        r del str
        set max 524288; # 64k
        set first_one_pos -1
        for {set j 0} {$j < 1000} {incr j} {
            assert {[r bitpos str 1] == $first_one_pos}
            assert {[r bitpos str 1 0 -1 bit] == $first_one_pos}
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
            if {$first_zero_pos == $max} {
                assert {[r bitpos str 0 0 -1 bit] == -1}
            } else {
                assert {[r bitpos str 0 0 -1 bit] == $first_zero_pos}
            }
            set pos [randomInt $max]
            r setbit str $pos 0
            if {$first_zero_pos > $pos} {
                # Update the position of the first 0 bit in the array
                # if the bit we clear is on the left of the previous one.
                set first_zero_pos $pos
            }
        }
    }

    # This test creates a string of 10 bytes. It has two iterations. One clears
    # all the bits and sets just one bit and another set all the bits and clears
    # just one bit. Each iteration loops from bit offset 0 to 79 and uses SETBIT
    # to set the bit to 0 or 1, and then use BITPOS and BITCOUNT on a few mutations.
    test {BITPOS/BITCOUNT fuzzy testing using SETBIT} {
        # We have two start and end ranges, each range used to select a random
        # position, one for start position and one for end position.
        proc test_one {start1 end1 start2 end2 pos bit pos_type} {
            set start [randomRange $start1 $end1]
            set end [randomRange $start2 $end2]
            if {$start > $end} {
                # Swap start and end
                lassign [list $end $start] start end
            }
            set startbit $start
            set endbit $end
            # For byte index, we need to generate the real bit index
            if {[string equal $pos_type byte]} {
                set startbit [expr $start << 3]
                set endbit [expr ($end << 3) + 7]
            }
            # This means whether the test bit index is in the range.
            set inrange [expr ($pos >= $startbit && $pos <= $endbit) ? 1: 0]
            # For bitcount, there are four different results.
            # $inrange == 0 && $bit == 0, all bits in the range are set, so $endbit - $startbit + 1
            # $inrange == 0 && $bit == 1, all bits in the range are clear, so 0
            # $inrange == 1 && $bit == 0, all bits in the range are set but one, so $endbit - $startbit
            # $inrange == 1 && $bit == 1, all bits in the range are clear but one, so 1
            set res_count [expr ($endbit - $startbit + 1) * (1 - $bit) + $inrange * [expr $bit ? 1 : -1]]
            assert {[r bitpos str $bit $start $end $pos_type] == [expr $inrange ? $pos : -1]}
            assert {[r bitcount str $start $end $pos_type] == $res_count}
        }

        r del str
        set max 80;
        r setbit str [expr $max - 1] 0
        set bytes [expr $max >> 3]
        # First iteration sets all bits to 1, then set bit to 0 from 0 to max - 1
        # Second iteration sets all bits to 0, then set bit to 1 from 0 to max - 1
        for {set bit 0} {$bit < 2} {incr bit} {
            r bitop not str str
            for {set j 0} {$j < $max} {incr j} {
                r setbit str $j $bit

                # First iteration tests byte index and second iteration tests bit index.
                foreach {curr end pos_type} [list [expr $j >> 3] $bytes byte $j $max bit] {
                    # start==end set to bit position
                    test_one $curr $curr $curr $curr $j $bit $pos_type
                    # Both start and end are before bit position
                    if {$curr > 0} {
                        test_one 0 $curr 0 $curr $j $bit $pos_type
                    }
                    # Both start and end are after bit position
                    if {$curr < [expr $end - 1]} {
                        test_one [expr $curr + 1] $end [expr $curr + 1] $end $j $bit $pos_type
                    }
                    # start is before and end is after bit position
                    if {$curr > 0 && $curr < [expr $end - 1]} {
                        test_one 0 $curr [expr $curr +1] $end $j $bit $pos_type
                    }
                }

                # restore bit
                r setbit str $j [expr 1 - $bit]
            }
        }
    }
}

run_solo {bitops-large-memory} {
start_server {tags {"bitops"}} {
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
    } {1} {large-memory}

    test "SETBIT values larger than UINT32_MAX and lzf_compress/lzf_decompress correctly" {
        set bytes [expr (1 << 32) + 1]
        set bitpos [expr (1 << 35)]
        set oldval [lindex [r config get proto-max-bulk-len] 1]
        r config set proto-max-bulk-len $bytes
        r setbit mykey $bitpos 1
        assert_equal $bytes [r strlen mykey]
        assert_equal 1 [r getbit mykey $bitpos]
        r debug reload ;# lzf_compress/lzf_decompress when RDB saving/loading.
        assert_equal 1 [r getbit mykey $bitpos]
        r config set proto-max-bulk-len $oldval
        r del mykey
    } {1} {large-memory needs:debug}
}
} ;#run_solo
