# Compare Redis commadns against Tcl implementations of the same commands.
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
        for {set j 1} {$j < $count} {incr j} {
            set bit2 [string range $b($j) $x $x]
            switch $op {
                and {set bit [expr {$bit & $bit2}]}
                or  {set bit [expr {$bit | $bit2}]}
                xor {set bit [expr {$bit ^ $bit2}]}
                not {set bit [expr {!$bit}]}
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

    catch {unset num}
    foreach vec [list "" "\xaa" "\x00\x00\xff" "foobar"] {
        incr num
        test "BITCOUNT against test vector #$num" {
            r set str $vec
            assert {[r bitcount str] == [count_bits $vec]}
        }
    }

    test {BITCOUNT fuzzing} {
        for {set j 0} {$j < 100} {incr j} {
            set str [randstring 0 3000]
            r set str $str
            assert {[r bitcount str] == [count_bits $str]}
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
}
