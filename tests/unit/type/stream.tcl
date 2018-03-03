# return value is like strcmp() and similar.
proc streamCompareID {a b} {
    if {$a eq $b} {return 0}
    lassign [split $a -] a_ms a_seq
    lassign [split $b -] b_ms b_seq
    if {$a_ms > $b_ms} {return 1}
    if {$a_ms < $b_ms} {return -1}
    # Same ms case, compare seq.
    if {$a_seq > $b_seq} {return 1}
    if {$a_seq < $b_seq} {return -1}
}

# return the ID immediately greater than the specified one.
# Note that this function does not care to handle 'seq' overflow
# since it's a 64 bit value.
proc streamNextID {id} {
    lassign [split $id -] ms seq
    incr seq
    join [list $ms $seq] -
}

# Generate a random stream entry ID with the ms part between min and max
# and a low sequence number (0 - 999 range), in order to stress test
# XRANGE against a Tcl implementation implementing the same concept
# with Tcl-only code in a linear array.
proc streamRandomID {min_id max_id} {
    lassign [split $min_id -] min_ms min_seq
    lassign [split $max_id -] max_ms max_seq
    set delta [expr {$max_ms-$min_ms+1}]
    set ms [expr {$min_ms+[randomInt $delta]}]
    set seq [randomInt 1000]
    return $ms-$seq
}

# Tcl-side implementation of XRANGE to perform fuzz testing in the Redis
# XRANGE implementation.
proc streamSimulateXRANGE {items start end} {
    set res {}
    foreach i $items  {
        set this_id [lindex $i 0]
        if {[streamCompareID $this_id $start] >= 0} {
            if {[streamCompareID $this_id $end] <= 0} {
                lappend res $i
            }
        }
    }
    return $res
}

set content {} ;# Will be populated with Tcl side copy of the stream content.

start_server {
    tags {"stream"}
} {
    test {XADD can add entries into a stream that XRANGE can fetch} {
        r XADD mystream * item 1 value a
        r XADD mystream * item 2 value b
        assert_equal 2 [r XLEN mystream]
        set items [r XRANGE mystream - +]
        assert_equal [lindex $items 0 1] {item 1 value a}
        assert_equal [lindex $items 1 1] {item 2 value b}
    }

    test {XADD IDs are incremental} {
        set id1 [r XADD mystream * item 1 value a]
        set id2 [r XADD mystream * item 2 value b]
        set id3 [r XADD mystream * item 3 value c]
        assert {[streamCompareID $id1 $id2] == -1}
        assert {[streamCompareID $id2 $id3] == -1}
    }

    test {XADD IDs are incremental when ms is the same as well} {
        r multi
        r XADD mystream * item 1 value a
        r XADD mystream * item 2 value b
        r XADD mystream * item 3 value c
        lassign [r exec] id1 id2 id3
        assert {[streamCompareID $id1 $id2] == -1}
        assert {[streamCompareID $id2 $id3] == -1}
    }

    test {XADD with MAXLEN option} {
        r DEL mystream
        for {set j 0} {$j < 1000} {incr j} {
            if {rand() < 0.9} {
                r XADD mystream MAXLEN 5 * xitem $j
            } else {
                r XADD mystream MAXLEN 5 * yitem $j
            }
        }
        set res [r xrange mystream - +]
        set expected 995
        foreach r $res {
            assert {[lindex $r 1 1] == $expected}
            incr expected
        }
    }

    test {XADD mass insertion and XLEN} {
        r DEL mystream
        r multi
        for {set j 0} {$j < 10000} {incr j} {
            # From time to time insert a field with a different set
            # of fields in order to stress the stream compression code.
            if {rand() < 0.9} {
                r XADD mystream * item $j
            } else {
                r XADD mystream * item $j otherfield foo
            }
        }
        r exec

        set items [r XRANGE mystream - +]
        for {set j 0} {$j < 10000} {incr j} {
            assert {[lrange [lindex $items $j 1] 0 1] eq [list item $j]}
        }
        assert {[r xlen mystream] == $j}
    }

    test {XRANGE COUNT works as expected} {
        assert {[llength [r xrange mystream - + COUNT 10]] == 10}
    }

    test {XREVRANGE COUNT works as expected} {
        assert {[llength [r xrevrange mystream + - COUNT 10]] == 10}
    }

    test {XRANGE can be used to iterate the whole stream} {
        set last_id "-"
        set j 0
        while 1 {
            set elements [r xrange mystream $last_id + COUNT 100]
            if {[llength $elements] == 0} break
            foreach e $elements {
                assert {[lrange [lindex $e 1] 0 1] eq [list item $j]}
                incr j;
            }
            set last_id [streamNextID [lindex $elements end 0]]
        }
        assert {$j == 10000}
    }

    test {XREVRANGE returns the reverse of XRANGE} {
        assert {[r xrange mystream - +] == [lreverse [r xrevrange mystream + -]]}
    }

    test {XREAD with non empty stream} {
        set res [r XREAD COUNT 1 STREAMS mystream 0.0]
        assert {[lrange [lindex $res 0 1 0 1] 0 1] eq {item 0}}
    }

    test {Non blocking XREAD with empty streams} {
        set res [r XREAD STREAMS s1 s2 0.0 0.0]
        assert {$res eq {}}
    }

    test {XREAD with non empty second stream} {
        set res [r XREAD COUNT 1 STREAMS nostream mystream 0.0 0.0]
        assert {[lindex $res 0 0] eq {mystream}}
        assert {[lrange [lindex $res 0 1 0 1] 0 1] eq {item 0}}
    }

    test {Blocking XREAD waiting new data} {
        r XADD s2 * old abcd1234
        set rd [redis_deferring_client]
        $rd XREAD BLOCK 20000 STREAMS s1 s2 s3 $ $ $
        r XADD s2 * new abcd1234
        set res [$rd read]
        assert {[lindex $res 0 0] eq {s2}}
        assert {[lindex $res 0 1 0 1] eq {new abcd1234}}
    }

    test {Blocking XREAD waiting old data} {
        set rd [redis_deferring_client]
        $rd XREAD BLOCK 20000 STREAMS s1 s2 s3 $ 0.0 $
        r XADD s2 * foo abcd1234
        set res [$rd read]
        assert {[lindex $res 0 0] eq {s2}}
        assert {[lindex $res 0 1 0 1] eq {old abcd1234}}
    }

    test "XREAD: XADD + DEL should not awake client" {
        set rd [redis_deferring_client]
        r del s1
        $rd XREAD BLOCK 20000 STREAMS s1 $
        r multi
        r XADD s1 * old abcd1234
        r DEL s1
        r exec
        r XADD s1 * new abcd1234
        set res [$rd read]
        assert {[lindex $res 0 0] eq {s1}}
        assert {[lindex $res 0 1 0 1] eq {new abcd1234}}
    }

    test "XREAD: XADD + DEL + LPUSH should not awake client" {
        set rd [redis_deferring_client]
        r del s1
        $rd XREAD BLOCK 20000 STREAMS s1 $
        r multi
        r XADD s1 * old abcd1234
        r DEL s1
        r LPUSH s1 foo bar
        r exec
        r DEL s1
        r XADD s1 * new abcd1234
        set res [$rd read]
        assert {[lindex $res 0 0] eq {s1}}
        assert {[lindex $res 0 1 0 1] eq {new abcd1234}}
    }

    test {XREAD with same stream name multiple times should work} {
        r XADD s2 * old abcd1234
        set rd [redis_deferring_client]
        $rd XREAD BLOCK 20000 STREAMS s2 s2 s2 $ $ $
        r XADD s2 * new abcd1234
        set res [$rd read]
        assert {[lindex $res 0 0] eq {s2}}
        assert {[lindex $res 0 1 0 1] eq {new abcd1234}}
    }

    test {XREAD + multiple XADD inside transaction} {
        r XADD s2 * old abcd1234
        set rd [redis_deferring_client]
        $rd XREAD BLOCK 20000 STREAMS s2 s2 s2 $ $ $
        r MULTI
        r XADD s2 * field one
        r XADD s2 * field two
        r XADD s2 * field three
        r EXEC
        set res [$rd read]
        assert {[lindex $res 0 0] eq {s2}}
        assert {[lindex $res 0 1 0 1] eq {field one}}
        assert {[lindex $res 0 1 1 1] eq {field two}}
    }

    test {XRANGE fuzzing} {
        set low_id [lindex $items 0 0]
        set high_id [lindex $items end 0]
        for {set j 0} {$j < 100} {incr j} {
            set start [streamRandomID $low_id $high_id]
            set end [streamRandomID $low_id $high_id]
            set range [r xrange mystream $start $end]
            set tcl_range [streamSimulateXRANGE $items $start $end]
            if {$range ne $tcl_range} {
                puts "*** WARNING *** - XRANGE fuzzing mismatch: $start - $end"
                puts "---"
                puts "XRANGE: '$range'"
                puts "---"
                puts "TCL: '$tcl_range'"
                puts "---"
                fail "XRANGE fuzzing failed, check logs for details"
            }
        }
    }
}
