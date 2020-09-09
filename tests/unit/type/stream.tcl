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

    test {XADD IDs correctly report an error when overflowing} {
        r DEL mystream
        r xadd mystream 18446744073709551615-18446744073709551615 a b
        assert_error ERR* {r xadd mystream * c d}
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

    test {XADD with ID 0-0} {
        r DEL otherstream
        catch {r XADD otherstream 0-0 k v} err
        assert {[r EXISTS otherstream] == 0}
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
        set res [r XREAD COUNT 1 STREAMS mystream 0-0]
        assert {[lrange [lindex $res 0 1 0 1] 0 1] eq {item 0}}
    }

    test {Non blocking XREAD with empty streams} {
        set res [r XREAD STREAMS s1 s2 0-0 0-0]
        assert {$res eq {}}
    }

    test {XREAD with non empty second stream} {
        set res [r XREAD COUNT 1 STREAMS nostream mystream 0-0 0-0]
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
        $rd XREAD BLOCK 20000 STREAMS s1 s2 s3 $ 0-0 $
        r XADD s2 * foo abcd1234
        set res [$rd read]
        assert {[lindex $res 0 0] eq {s2}}
        assert {[lindex $res 0 1 0 1] eq {old abcd1234}}
    }

    test {Blocking XREAD will not reply with an empty array} {
        r del s1
        r XADD s1 666 f v
        r XADD s1 667 f2 v2
        r XDEL s1 667
        set rd [redis_deferring_client]
        $rd XREAD BLOCK 10 STREAMS s1 666
        after 20
        assert {[$rd read] == {}} ;# before the fix, client didn't even block, but was served synchronously with {s1 {}}
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

    test {XDEL basic test} {
        r del somestream
        r xadd somestream * foo value0
        set id [r xadd somestream * foo value1]
        r xadd somestream * foo value2
        r xdel somestream $id
        assert {[r xlen somestream] == 2}
        set result [r xrange somestream - +]
        assert {[lindex $result 0 1 1] eq {value0}}
        assert {[lindex $result 1 1 1] eq {value2}}
    }

    # Here the idea is to check the consistency of the stream data structure
    # as we remove all the elements down to zero elements.
    test {XDEL fuzz test} {
        r del somestream
        set ids {}
        set x 0; # Length of the stream
        while 1 {
            lappend ids [r xadd somestream * item $x]
            incr x
            # Add enough elements to have a few radix tree nodes inside the stream.
            if {[dict get [r xinfo stream somestream] radix-tree-keys] > 20} break
        }

        # Now remove all the elements till we reach an empty stream
        # and after every deletion, check that the stream is sane enough
        # to report the right number of elements with XRANGE: this will also
        # force accessing the whole data structure to check sanity.
        assert {[r xlen somestream] == $x}

        # We want to remove elements in random order to really test the
        # implementation in a better way.
        set ids [lshuffle $ids]
        foreach id $ids {
            assert {[r xdel somestream $id] == 1}
            incr x -1
            assert {[r xlen somestream] == $x}
            # The test would be too slow calling XRANGE for every iteration.
            # Do it every 100 removal.
            if {$x % 100 == 0} {
                set res [r xrange somestream - +]
                assert {[llength $res] == $x}
            }
        }
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

    test {XREVRANGE regression test for issue #5006} {
        # Add non compressed entries
        r xadd teststream 1234567891230 key1 value1
        r xadd teststream 1234567891240 key2 value2
        r xadd teststream 1234567891250 key3 value3

        # Add SAMEFIELD compressed entries
        r xadd teststream2 1234567891230 key1 value1
        r xadd teststream2 1234567891240 key1 value2
        r xadd teststream2 1234567891250 key1 value3

        assert_equal [r xrevrange teststream 1234567891245 -] {{1234567891240-0 {key2 value2}} {1234567891230-0 {key1 value1}}}

        assert_equal [r xrevrange teststream2 1234567891245 -] {{1234567891240-0 {key1 value2}} {1234567891230-0 {key1 value1}}}
    }

    test {XREAD streamID edge (no-blocking)} {
        r del x
        r XADD x 1-1 f v
        r XADD x 1-18446744073709551615 f v
        r XADD x 2-1 f v
        set res [r XREAD BLOCK 0 STREAMS x 1-18446744073709551615]
        assert {[lindex $res 0 1 0] == {2-1 {f v}}}
    }

    test {XREAD streamID edge (blocking)} {
        r del x
        set rd [redis_deferring_client]
        $rd XREAD BLOCK 0 STREAMS x 1-18446744073709551615
        r XADD x 1-1 f v
        r XADD x 1-18446744073709551615 f v
        r XADD x 2-1 f v
        set res [$rd read]
        assert {[lindex $res 0 1 0] == {2-1 {f v}}}
    }

    test {XADD streamID edge} {
        r del x
        r XADD x 2577343934890-18446744073709551615 f v ;# we need the timestamp to be in the future
        r XADD x * f2 v2
        assert_equal [r XRANGE x - +] {{2577343934890-18446744073709551615 {f v}} {2577343934891-0 {f2 v2}}}
    }
}

start_server {tags {"stream"} overrides {appendonly yes}} {
    test {XADD with MAXLEN > xlen can propagate correctly} {
        for {set j 0} {$j < 100} {incr j} {
            r XADD mystream * xitem v
        }
        r XADD mystream MAXLEN 200 * xitem v
        incr j
        assert {[r xlen mystream] == $j}
        r debug loadaof
        r XADD mystream * xitem v
        incr j
        assert {[r xlen mystream] == $j}
    }
}

start_server {tags {"stream"} overrides {appendonly yes}} {
    test {XADD with ~ MAXLEN can propagate correctly} {
        for {set j 0} {$j < 100} {incr j} {
            r XADD mystream * xitem v
        }
        r XADD mystream MAXLEN ~ $j * xitem v
        incr j
        assert {[r xlen mystream] == $j}
        r config set stream-node-max-entries 1
        r debug loadaof
        r XADD mystream * xitem v
        incr j
        assert {[r xlen mystream] == $j}
    }
}

start_server {tags {"stream"} overrides {appendonly yes stream-node-max-entries 10}} {
    test {XTRIM with ~ MAXLEN can propagate correctly} {
        for {set j 0} {$j < 100} {incr j} {
            r XADD mystream * xitem v
        }
        r XTRIM mystream MAXLEN ~ 85
        assert {[r xlen mystream] == 90}
        r config set stream-node-max-entries 1
        r debug loadaof
        r XADD mystream * xitem v
        incr j
        assert {[r xlen mystream] == 91}
    }
}

start_server {tags {"stream xsetid"}} {
    test {XADD can CREATE an empty stream} {
        r XADD mystream MAXLEN 0 * a b
        assert {[dict get [r xinfo stream mystream] length] == 0}
    }

    test {XSETID can set a specific ID} {
        r XSETID mystream "200-0"
        assert {[dict get [r xinfo stream mystream] last-generated-id] == "200-0"}
    }

    test {XSETID cannot SETID with smaller ID} {
        r XADD mystream * a b
        catch {r XSETID mystream "1-1"} err
        r XADD mystream MAXLEN 0 * a b
        set err
    } {ERR*smaller*}

    test {XSETID cannot SETID on non-existent key} {
        catch {r XSETID stream 1-1} err
        set _ $err
    } {ERR no such key}
}

start_server {tags {"stream"} overrides {appendonly yes aof-use-rdb-preamble no}} {
    test {Empty stream can be rewrite into AOF correctly} {
        r XADD mystream MAXLEN 0 * a b
        assert {[dict get [r xinfo stream mystream] length] == 0}
        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof
        assert {[dict get [r xinfo stream mystream] length] == 0}
    }

    test {Stream can be rewrite into AOF correctly after XDEL lastid} {
        r XSETID mystream 0-0
        r XADD mystream 1-1 a b
        r XADD mystream 2-2 a b
        assert {[dict get [r xinfo stream mystream] length] == 2}
        r XDEL mystream 2-2
        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof
        assert {[dict get [r xinfo stream mystream] length] == 1}
        assert {[dict get [r xinfo stream mystream] last-generated-id] == "2-2"}
    }
}

start_server {tags {"stream"}} {
    test {XGROUP HELP should not have unexpected options} {
        catch {r XGROUP help xxx} e
        assert_match "*Unknown subcommand or wrong number of arguments*" $e
    }
}
