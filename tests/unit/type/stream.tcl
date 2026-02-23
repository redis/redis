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
    test "XADD wrong number of args" {
        assert_error {*wrong number of arguments for 'xadd' command} {r XADD mystream}
        assert_error {*wrong number of arguments for 'xadd' command} {r XADD mystream *}
        assert_error {*wrong number of arguments for 'xadd' command} {r XADD mystream * field}
    }

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

    test {XADD auto-generated sequence is incremented for last ID} {
        r DEL mystream
        set id1 [r XADD mystream 123-456 item 1 value a]
        set id2 [r XADD mystream 123-* item 2 value b]
        lassign [split $id2 -] _ seq
        assert {$seq == 457}
        assert {[streamCompareID $id1 $id2] == -1}
    }

    test {XADD auto-generated sequence is zero for future timestamp ID} {
        r DEL mystream
        set id1 [r XADD mystream 123-456 item 1 value a]
        set id2 [r XADD mystream 789-* item 2 value b]
        lassign [split $id2 -] _ seq
        assert {$seq == 0}
        assert {[streamCompareID $id1 $id2] == -1}
    }

    test {XADD auto-generated sequence can't be smaller than last ID} {
        r DEL mystream
        r XADD mystream 123-456 item 1 value a
        assert_error ERR* {r XADD mystream 42-* item 2 value b}
    }

    test {XADD auto-generated sequence can't overflow} {
        r DEL mystream
        r xadd mystream 1-18446744073709551615 a b
        assert_error ERR* {r xadd mystream 1-* c d}
    }

    test {XADD 0-* should succeed} {
        r DEL mystream
        set id [r xadd mystream 0-* a b]
        lassign [split $id -] _ seq
        assert {$seq == 1}
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
        assert {[r xlen mystream] == 5}
        set res [r xrange mystream - +]
        set expected 995
        foreach r $res {
            assert {[lindex $r 1 1] == $expected}
            incr expected
        }
    }

    test {XADD with MAXLEN option and the '=' argument} {
        r DEL mystream
        for {set j 0} {$j < 1000} {incr j} {
            if {rand() < 0.9} {
                r XADD mystream MAXLEN = 5 * xitem $j
            } else {
                r XADD mystream MAXLEN = 5 * yitem $j
            }
        }
        assert {[r XLEN mystream] == 5}
    }

    test {XADD with MAXLEN option and the '~' argument} {
        r DEL mystream
        r config set stream-node-max-entries 100
        for {set j 0} {$j < 1000} {incr j} {
            if {rand() < 0.9} {
                r XADD mystream MAXLEN ~ 555 * xitem $j
            } else {
                r XADD mystream MAXLEN ~ 555 * yitem $j
            }
        }
        assert {[r XLEN mystream] == 600}
    }

    test {XADD with NOMKSTREAM option} {
        r DEL mystream
        assert_equal "" [r XADD mystream NOMKSTREAM * item 1 value a]
        assert_equal 0 [r EXISTS mystream]
        r XADD mystream * item 1 value a
        r XADD mystream NOMKSTREAM * item 2 value b
        assert_equal 2 [r XLEN mystream]
        set items [r XRANGE mystream - +]
        assert_equal [lindex $items 0 1] {item 1 value a}
        assert_equal [lindex $items 1 1] {item 2 value b}
    }

    test {XADD with MINID option} {
        r DEL mystream
        for {set j 1} {$j < 1001} {incr j} {
            set minid 1000
            if {$j >= 5} {
                set minid [expr {$j-5}]
            }
            if {rand() < 0.9} {
                r XADD mystream MINID $minid $j xitem $j
            } else {
                r XADD mystream MINID $minid $j yitem $j
            }
        }
        assert {[r xlen mystream] == 6}
        set res [r xrange mystream - +]
        set expected 995
        foreach r $res {
            assert {[lindex $r 1 1] == $expected}
            incr expected
        }
    }

    test {XADD with MAXLEN option and ACKED option} {
        r DEL mystream
        r XADD mystream 1-0 f v
        r XADD mystream 2-0 f v
        r XADD mystream 3-0 f v
        r XADD mystream 4-0 f v
        r XADD mystream 5-0 f v
        assert {[r XLEN mystream] == 5}

        # Create a consumer group but don't read any messages yet
        # ACKED option should preserve all messages since none are acked.
        r XGROUP CREATE mystream mygroup 0
        r XADD mystream MAXLEN = 1 ACKED 6-0 f v
        assert {[r XLEN mystream] == 6} ;# All messages preserved + the new one

        # Read 1 messages and acknowledge them
        # This leaves 5 messages still unacked
        set records [r XREADGROUP GROUP mygroup consumer1 COUNT 1 STREAMS mystream >]
        r XACK mystream mygroup [lindex [lindex [lindex [lindex $records 0] 1] 0] 0]
        assert {[lindex [r XPENDING mystream mygroup] 0] == 0}

        # With 5 messages still unacked, ACKED option should preserve them
        r XADD mystream MAXLEN = 1 ACKED 7-0 f v
        assert {[r XLEN mystream] == 6} ;# 6 - 1 acked + 1 new

        # Acknowledge all remaining messages
        set records [r XREADGROUP GROUP mygroup consumer1 STREAMS mystream >]
        set ids {}
        foreach entry [lindex [lindex $records 0] 1] {
            lappend ids [lindex $entry 0]
        }
        r XACK mystream mygroup {*}$ids
        assert {[lindex [r XPENDING mystream mygroup] 0] == 0} ;# All messages acked

        # Now ACKED should trim to MAXLEN since all messages are acked
        r XADD mystream MAXLEN = 1 ACKED * f v
        assert {[r XLEN mystream] == 1} ;# Successfully trimmed to 1 entries
    }

    test {XADD with ACKED option doesn't crash after DEBUG RELOAD} {
        r DEL mystream
        r XADD mystream 1-0 f v

        # Create a consumer group and read one message
        r XGROUP CREATE mystream mygroup 0
        set records [r XREADGROUP GROUP mygroup consumer1 COUNT 1 STREAMS mystream >]
        assert_equal [lindex [r XPENDING mystream mygroup] 0] 1

        # After reload, the reference relationship between consumer groups and messages
        # is correctly rebuilt, so the previously read but unacked message still cannot be deleted.
        r DEBUG RELOAD
        r XADD mystream MAXLEN = 1 ACKED 2-0 f v
        assert_equal [r XLEN mystream] 2

        # Acknowledge the read message so the PEL becomes empty
        r XACK mystream mygroup [lindex [lindex [lindex [lindex $records 0] 1] 0] 0]
        assert {[lindex [r XPENDING mystream mygroup] 0] == 0}

        # After reload, since PEL is empty, no cgroup references will be recreated.
        r DEBUG RELOAD

        # ACKED option should work correctly even without cgroup references.
        r XADD mystream MAXLEN = 1 ACKED 3-0 f v
        assert_equal [r XLEN mystream] 2
    } {} {needs:debug}

    test {XADD with MAXLEN option and DELREF option} {
        r DEL mystream
        r XADD mystream 1-0 f v
        r XADD mystream 2-0 f v
        r XADD mystream 3-0 f v
        r XADD mystream 4-0 f v
        r XADD mystream 5-0 f v

        r XGROUP CREATE mystream mygroup 0
        r XREADGROUP GROUP mygroup consumer1 COUNT 1 STREAMS mystream >

        # XADD with MAXLEN and DELREF should trim and remove all references
        r XADD mystream MAXLEN = 1 DELREF * f v
        assert {[r XLEN mystream] == 1}

        # All PEL entries should be cleaned up
        assert {[lindex [r XPENDING mystream mygroup] 0] == 0}
    }

    test {XADD IDMP with invalid syntax} {
        r DEL mystream
        assert_error "*ERR Invalid stream ID specified*" {r XADD mystream IDMP p1 * f v}
        assert_error "*IDMP/IDMPAUTO can be used only with auto-generated IDs*" {r XADD mystream IDMP p1 iid1 1-1 f v}
        assert_error "*IDMP/IDMPAUTO specified multiple times*" {r XADD mystream IDMP p1 iid1 IDMP p2 iid2 * f v}
        assert_error "*IDMP/IDMPAUTO specified multiple times*" {r XADD mystream IDMPAUTO p1 IDMP p2 iid2 * f v}
        assert_error "*IDMP requires a non-empty producer ID*" {r XADD mystream IDMP "" iid1 * f v}
        assert_error "*IDMP requires a non-empty idempotent ID*" {r XADD mystream IDMP p1 "" * f v}
        assert_error "*IDMPAUTO requires a non-empty producer ID*" {r XADD mystream IDMPAUTO "" * f v}
    }

    test {XADD IDMP basic addition} {
        r DEL mystream
    
        assert {[regexp {^[0-9]+-[0-9]+$} [r XADD mystream IDMP p1 1 * f v]]}
        assert {[regexp {^[0-9]+-[0-9]+$} [r XADD mystream IDMP p1 A * f v]]}
        assert {[regexp {^[0-9]+-[0-9]+$} [r XADD mystream IDMP p1 B * f v]]}
        assert {[regexp {^[0-9]+-[0-9]+$} [r XADD mystream IDMP p1 - * f v]]}
        assert {[regexp {^[0-9]+-[0-9]+$} [r XADD mystream IDMP p1 + * f v]]}
        assert {[regexp {^[0-9]+-[0-9]+$} [r XADD mystream IDMP p1 * * f v]]}
        assert {[regexp {^[0-9]+-[0-9]+$} [r XADD mystream IDMP p1 ^ * f v]]}
        assert {[regexp {^[0-9]+-[0-9]+$} [r XADD mystream IDMP p1 $ * f v]]}
        assert {[regexp {^[0-9]+-[0-9]+$} [r XADD mystream IDMP p1 # * f v]]}
        assert {[regexp {^[0-9]+-[0-9]+$} [r XADD mystream IDMP p1 @ * f v]]}
        assert {[regexp {^[0-9]+-[0-9]+$} [r XADD mystream IDMP p1 ? * f v]]}
        assert {[regexp {^[0-9]+-[0-9]+$} [r XADD mystream IDMP p1 \\ * f v]]}
        assert {[regexp {^[0-9]+-[0-9]+$} [r XADD mystream IDMP p1 IDMP * f v]]}
        assert {[regexp {^[0-9]+-[0-9]+$} [r XADD mystream IDMP p1 123-456 * f v]]}
        
        assert {[regexp {^[0-9]+-[0-9]+$} [r XADD mystream IDMP p1 9999999999999-9999999999999 * f v]]}
        assert {[regexp {^[0-9]+-[0-9]+$} [r XADD mystream IDMP p1 "hello世界" * f v]]}
        assert {[regexp {^[0-9]+-[0-9]+$} [r XADD mystream IDMP p1 "héllo" * f v]]}
        
        assert {[regexp {^[0-9]+-[0-9]+$} [r XADD mystream IDMP p1 "line1\nline2" * f v]]}
        assert {[regexp {^[0-9]+-[0-9]+$} [r XADD mystream IDMP p1 "tab\there" * f v]]}
        assert {[regexp {^[0-9]+-[0-9]+$} [r XADD mystream IDMP p1 "quote\"test" * f v]]}
        assert {[regexp {^[0-9]+-[0-9]+$} [r XADD mystream IDMP p1 "with spaces" * f v]]}
        
        assert {[regexp {^[0-9]+-[0-9]+$} [r XADD mystream IDMP p1 [string repeat "long" 100] * f v]]}
        assert {[regexp {^[0-9]+-[0-9]+$} [r XADD mystream IDMP p1 [string repeat "x" 1000] * f v]]}
        
        assert {[regexp {^[0-9]+-[0-9]+$} [r XADD mystream IDMP p1 "special!@#$%^&*()" * f v]]}
        assert {[regexp {^[0-9]+-[0-9]+$} [r XADD mystream IDMP p1 "path/to/file" * f v]]}
        assert {[regexp {^[0-9]+-[0-9]+$} [r XADD mystream IDMP p1 "key:value" * f v]]}
        
        assert_equal 26 [r XLEN mystream]
    }

    test "XADD IDMP duplicate request returns same ID" {
        r DEL mystream
        
        # First XADD with IDMP
        set id1 [r XADD mystream IDMP p1 "payment-abc" * amount "100" currency "USD"]
        
        # Second XADD with same iid but different fields
        set id2 [r XADD mystream IDMP p1 "payment-abc" * amount "200" currency "EUR"]
        
        # Verify both IDs are identical
        assert_equal $id1 $id2
        
        # Verify only one entry exists
        assert_equal 1 [r XLEN mystream]
        
        # Verify original fields are preserved
        set entries [r XRANGE mystream - +]
        set fields [lindex [lindex $entries 0] 1]
        assert_equal "100" [dict get $fields amount]
        assert_equal "USD" [dict get $fields currency]
    }

    test {XADD IDMP multiple different IIDs create multiple entries} {
        r DEL mystream
        
        # Add entries with different iids
        set id1 [r XADD mystream IDMP p1 "req-1" * user "alice"]
        set id2 [r XADD mystream IDMP p1 "req-2" * user "bob"]
        set id3 [r XADD mystream IDMP p1 "req-3" * user "charlie"]
        
        # Verify all IDs are different
        assert {$id1 != $id2}
        assert {$id2 != $id3}
        assert {$id1 != $id3}
        
        # Verify all entries exist
        assert_equal 3 [r XLEN mystream]
        
        # Verify each entry has correct data
        set entries [r XRANGE mystream - +]
        assert_equal "alice" [dict get [lindex [lindex $entries 0] 1] user]
        assert_equal "bob" [dict get [lindex [lindex $entries 1] 1] user]
        assert_equal "charlie" [dict get [lindex [lindex $entries 2] 1] user]
    }

    test {XADD IDMP with binary-safe iid} {
        r DEL mystream
        
        # Test with null bytes and binary data
        set binary_iid "\x00\x01\x02\xff"
        set id1 [r XADD mystream IDMP p1 $binary_iid * field "value"]
        set id2 [r XADD mystream IDMP p1 $binary_iid * field "dup"]
        assert_equal $id1 $id2
    }

    test {XADD IDMP with maximum length iid} {
        r DEL mystream
        
        # Test with very long iid (e.g., 64KB)
        set long_iid [string repeat "x" 65536]
        set id [r XADD mystream IDMP p1 $long_iid * field "value"]
        assert_match {*-*} $id
    }

    test {XADD IDMP with MAXLEN option} {
        r DEL mystream
        
        # Add entries with IDMP and MAXLEN
        set id1 [r XADD mystream IDMP p1 "req-1" MAXLEN ~ 100 * field "value1"]
        set id2 [r XADD mystream IDMP p1 "req-2" MAXLEN ~ 100 * field "value2"]
        
        # Attempt duplicate
        set id1_dup [r XADD mystream IDMP p1 "req-1" MAXLEN ~ 100 * field "value3"]
        
        # Verify deduplication works
        assert_equal $id1 $id1_dup
        
        # Verify only 2 entries exist
        assert_equal 2 [r XLEN mystream]
    }

    test {XADD IDMP with MINID option} {
        r DEL mystream
        
        # Add entry with IDMP and MINID
        set id1 [r XADD mystream IDMP p1 "req-1" MINID ~ 1000000000-0 * field "value1"]
        
        # Attempt duplicate with MINID
        set id2 [r XADD mystream IDMP p1 "req-1" MINID ~ 1000000000-0 * field "value2"]
        
        # Verify deduplication works
        assert_equal $id1 $id2
        assert_equal 1 [r XLEN mystream]
    }

    test {XADD IDMP with NOMKSTREAM option} {
        r DEL mystream
        
        # Attempt XADD with NOMKSTREAM on non-existent stream
        set result [r XADD mystream NOMKSTREAM IDMP p1 "req-1" * field "value"]
        assert_equal {} $result
        
        # Create stream normally
        r XADD mystream IDMP p1 "req-2" * field "value"
        
        # Now NOMKSTREAM should work
        set id [r XADD mystream NOMKSTREAM IDMP p1 "req-3" * field "value"]
        assert_match {*-*} $id
        
        assert_equal 2 [r XLEN mystream]
    }

    test {XADD IDMP with KEEPREF option} {
        r DEL mystream
        
        # Add entry with IDMP and KEEPREF
        set id1 [r XADD mystream IDMP p1 "req-1" KEEPREF * field "value1"]
        
        # Attempt duplicate with KEEPREF
        set id2 [r XADD mystream IDMP p1 "req-1" KEEPREF * field "value2"]
        
        # Verify deduplication works
        assert_equal $id1 $id2
        assert_equal 1 [r XLEN mystream]
    }

    test {XADD IDMP with combined options} {
        r DEL mystream
        
        # Add entry with all options
        set id1 [r XADD mystream IDMP p1 "req-1" KEEPREF MAXLEN ~ 1000 LIMIT 10 * field1 "value1" field2 "value2"]
        
        # Attempt duplicate with all options
        set id2 [r XADD mystream IDMP p1 "req-1" KEEPREF MAXLEN ~ 1000 LIMIT 10 * field3 "value3"]
        
        # Verify deduplication works
        assert_equal $id1 $id2
        assert_equal 1 [r XLEN mystream]
        
        # Verify original fields preserved
        set entries [r XRANGE mystream - +]
        set fields [lindex [lindex $entries 0] 1]
        assert_equal "value1" [dict get $fields field1]
        assert_equal "value2" [dict get $fields field2]
    }

    test {XADD IDMP argument order variations} {
        r DEL mystream
        
        # IDMP before MAXLEN
        set id1 [r XADD mystream IDMP p1 "req-1" MAXLEN ~ 100 * field "value1"]
        
        # IDMP after MAXLEN
        set id2 [r XADD mystream MAXLEN ~ 100 IDMP p1 "req-2" * field "value2"]
        
        # Multiple options in different order
        set id3 [r XADD mystream NOMKSTREAM IDMP p1 "req-3" MAXLEN ~ 100 * field "value3"]
        
        # All should succeed
        assert_match {*-*} $id1
        assert_match {*-*} $id2
        assert_match {*-*} $id3
        
        assert_equal 3 [r XLEN mystream]
    }

    test {XADD IDMP concurrent duplicate requests} {
        r DEL mystream
        
        # Create multiple clients
        set client1 [redis_client]
        set client2 [redis_client]
        set client3 [redis_client]
        
        # Send same IDMP request from all clients concurrently
        set id1 [$client1 XADD mystream IDMP p1 "concurrent-req" * client "1"]
        set id2 [$client2 XADD mystream IDMP p1 "concurrent-req" * client "2"]
        set id3 [$client3 XADD mystream IDMP p1 "concurrent-req" * client "3"]
        
        # All should return the same ID
        assert_equal $id1 $id2
        assert_equal $id2 $id3
        
        # Only one entry should exist
        assert_equal 1 [r XLEN mystream]
        
        # Cleanup
        $client1 close
        $client2 close
        $client3 close
    }

    test {XADD IDMP pipelined requests} {
        r DEL mystream
        
        # Send pipelined requests
        r MULTI
        r XADD mystream IDMP p1 "req-1" * field "value1"
        r XADD mystream IDMP p1 "req-2" * field "value2"
        r XADD mystream IDMP p1 "req-1" * field "value3"  # Duplicate
        r XADD mystream IDMP p1 "req-3" * field "value4"
        set results [r EXEC]
        
        # Extract IDs
        set id1 [lindex $results 0]
        set id2 [lindex $results 1]
        set id1_dup [lindex $results 2]
        set id3 [lindex $results 3]
        
        # Verify deduplication
        assert_equal $id1 $id1_dup
        
        # Verify all IDs are different (except duplicate)
        assert {$id1 != $id2}
        assert {$id2 != $id3}
        assert {$id1 != $id3}
        
        # Verify only 3 entries exist
        assert_equal 3 [r XLEN mystream]
    }

    test {XADD IDMP with consumer groups} {
        r DEL mystream
        
        # Add entries with IDMP
        set id1 [r XADD mystream IDMP p1 "cg-1" * field "value1"]
        set id2 [r XADD mystream IDMP p1 "cg-2" * field "value2"]
        
        # Create consumer group
        r XGROUP CREATE mystream mygroup 0
        
        # Read entries
        set entries [r XREADGROUP GROUP mygroup consumer1 COUNT 10 STREAMS mystream >]
        
        # Verify both entries are readable
        set stream_entries [lindex [lindex $entries 0] 1]
        assert_equal 2 [llength $stream_entries]
        
        # ACK entries
        assert_equal 2 [r XACK mystream mygroup $id1 $id2]
        
        # Verify deduplication still works
        set id1_dup [r XADD mystream IDMP p1 "cg-1" * field "dup"]
        assert_equal $id1 $id1_dup
    }

    test {XADD IDMP persists in RDB} {
        r DEL mystream

        # Add entries with IDMP
        set id1 [r XADD mystream IDMP p1 "persist-1" * field "value1"]
        r XADD mystream IDMP p1 "persist-2" * field "value2"

        # Force RDB save
        r SAVE
        r DEBUG RELOAD

        # Verify stream still exists
        assert_equal 2 [r XLEN mystream]

        # Verify deduplication still works after restart
        set id1_dup [r XADD mystream IDMP p1 "persist-1" * field "new"]
        assert_equal $id1 $id1_dup

        # Should still have only 2 entries
        assert_equal 2 [r XLEN mystream]
    } {} {external:skip needs:debug}

    test {XADD IDMP set in AOF} {
        r DEL mystream
        r config set appendonly yes

        # Wait for the automatic AOF rewrite triggered by enabling AOF
        waitForBgrewriteaof r

        # Add entries with IDMP
        set id1 [r XADD mystream IDMP p1 "aof-1" * field "value1"]
        r XADD mystream IDMP p1 "aof-2" * field "value2"

        # Add duplicate
        set id1_dup [r XADD mystream IDMP p1 "aof-1" * field "dup"]
        assert_equal $id1 $id1_dup

        # Restart with AOF
        r DEBUG RELOAD

        # Verify stream exists
        assert_equal 2 [r XLEN mystream]

        # Verify deduplication still works
        set id1_dup2 [r XADD mystream IDMP p1 "aof-1" * field "new"]
        assert_equal $id1 $id1_dup2
    } {} {external:skip needs:debug}

    test {XADD IDMP multiple producers have isolated namespaces} {
        r DEL mystream
        
        # Add entry with producer p1
        set id1 [r XADD mystream IDMP producer1 "req-123" * field "from-p1"]
        
        # Add entry with same IID but different producer p2 - should create NEW entry
        set id2 [r XADD mystream IDMP producer2 "req-123" * field "from-p2"]
        
        # IDs should be different since producers are isolated
        assert {$id1 ne $id2}
        
        # Both entries should exist
        assert_equal 2 [r XLEN mystream]
        
        # Verify each entry has correct data
        set entries [r XRANGE mystream - +]
        assert_equal "from-p1" [dict get [lindex [lindex $entries 0] 1] field]
        assert_equal "from-p2" [dict get [lindex [lindex $entries 1] 1] field]
        
        # Duplicate within same producer should still deduplicate
        set id1_dup [r XADD mystream IDMP producer1 "req-123" * field "dup-p1"]
        assert_equal $id1 $id1_dup
        
        set id2_dup [r XADD mystream IDMP producer2 "req-123" * field "dup-p2"]
        assert_equal $id2 $id2_dup
        
        # Still only 2 entries
        assert_equal 2 [r XLEN mystream]
    }

    test {XADD IDMP multiple producers each have their own MAXSIZE limit} {
        r DEL mystream
        
        # Create stream and set global MAXSIZE
        r XADD mystream IDMP p1 "init" * field "init"
        r XCFGSET mystream IDMP-MAXSIZE 3 IDMP-DURATION 60
        
        # Add entries for producer p1 (will have 3: init, req-1, req-2, then req-3 evicts init)
        set p1_id1 [r XADD mystream IDMP p1 "req-1" * field "p1-v1"]
        set p1_id2 [r XADD mystream IDMP p1 "req-2" * field "p1-v2"]
        set p1_id3 [r XADD mystream IDMP p1 "req-3" * field "p1-v3"]
        
        # Add entries for producer p2 (separate tracking)
        set p2_id1 [r XADD mystream IDMP p2 "req-1" * field "p2-v1"]
        set p2_id2 [r XADD mystream IDMP p2 "req-2" * field "p2-v2"]
        set p2_id3 [r XADD mystream IDMP p2 "req-3" * field "p2-v3"]
        
        # p1's oldest entries should be evicted, but p1 req-1,2,3 should still work
        assert_equal $p1_id1 [r XADD mystream IDMP p1 "req-1" * field "dup"]
        assert_equal $p1_id2 [r XADD mystream IDMP p1 "req-2" * field "dup"]
        assert_equal $p1_id3 [r XADD mystream IDMP p1 "req-3" * field "dup"]
        
        # p2's entries should also still work (each producer has own MAXSIZE tracking)
        assert_equal $p2_id1 [r XADD mystream IDMP p2 "req-1" * field "dup"]
        assert_equal $p2_id2 [r XADD mystream IDMP p2 "req-2" * field "dup"]
        assert_equal $p2_id3 [r XADD mystream IDMP p2 "req-3" * field "dup"]
        
        # Verify pids-tracked
        set reply [r XINFO STREAM mystream]
        assert_equal 2 [dict get $reply pids-tracked]
    }

    test {XADD IDMP multiple producers with binary producer IDs} {
        r DEL mystream
        
        # Test with binary producer IDs
        set bin_pid1 "\x00\x01\x02"
        set bin_pid2 "\x03\x04\x05"
        
        set id1 [r XADD mystream IDMP $bin_pid1 "req-1" * field "v1"]
        set id2 [r XADD mystream IDMP $bin_pid2 "req-1" * field "v2"]
        
        # Different binary PIDs should be isolated
        assert {$id1 ne $id2}
        assert_equal 2 [r XLEN mystream]
        
        # Verify deduplication within same binary PID
        set id1_dup [r XADD mystream IDMP $bin_pid1 "req-1" * field "dup"]
        assert_equal $id1 $id1_dup
    }

    test {XADD IDMP multiple producers with unicode producer IDs} {
        r DEL mystream
        
        # Test with unicode producer IDs
        set id1 [r XADD mystream IDMP "producer-世界" "req-1" * field "v1"]
        set id2 [r XADD mystream IDMP "producer-héllo" "req-1" * field "v2"]
        set id3 [r XADD mystream IDMP "producer-日本" "req-1" * field "v3"]
        
        # All should be separate entries
        assert {$id1 ne $id2}
        assert {$id2 ne $id3}
        assert_equal 3 [r XLEN mystream]
        
        set reply [r XINFO STREAM mystream]
        assert_equal 3 [dict get $reply pids-tracked]
    }

    test {XADD IDMP multiple producers with long producer IDs} {
        r DEL mystream
        
        # Test with very long producer IDs
        set long_pid1 [string repeat "a" 1000]
        set long_pid2 [string repeat "b" 1000]
        
        set id1 [r XADD mystream IDMP $long_pid1 "req-1" * field "v1"]
        set id2 [r XADD mystream IDMP $long_pid2 "req-1" * field "v2"]
        
        # Different long PIDs should be isolated
        assert {$id1 ne $id2}
        assert_equal 2 [r XLEN mystream]
        
        # Verify deduplication
        set id1_dup [r XADD mystream IDMP $long_pid1 "req-1" * field "dup"]
        assert_equal $id1 $id1_dup
    }

    test {XADD IDMP multiple producers persistence in RDB} {
        r DEL mystream
        
        # Add entries with multiple producers
        set id1 [r XADD mystream IDMP p1 "req-1" * field "v1"]
        set id2 [r XADD mystream IDMP p2 "req-1" * field "v2"]
        set id3 [r XADD mystream IDMP p3 "req-1" * field "v3"]
        
        # Verify before save
        set reply [r XINFO STREAM mystream]
        assert_equal 3 [dict get $reply pids-tracked]
        assert_equal 3 [dict get $reply iids-tracked]
        
        # Save and reload
        r SAVE
        restart_server 0 true false
        
        # Verify after reload
        set reply [r XINFO STREAM mystream]
        assert_equal 3 [dict get $reply pids-tracked]
        assert_equal 3 [dict get $reply iids-tracked]
        
        # Verify deduplication still works for all producers
        assert_equal $id1 [r XADD mystream IDMP p1 "req-1" * field "dup"]
        assert_equal $id2 [r XADD mystream IDMP p2 "req-1" * field "dup"]
        assert_equal $id3 [r XADD mystream IDMP p3 "req-1" * field "dup"]
    } {} {external:skip}

    test {XADD IDMP multiple producers concurrent access} {
        r DEL mystream
        
        # Create multiple clients
        set client1 [redis_client]
        set client2 [redis_client]
        set client3 [redis_client]
        
        # Each client acts as a different producer
        set id1 [$client1 XADD mystream IDMP service-a "order-123" * data "from-a"]
        set id2 [$client2 XADD mystream IDMP service-b "order-123" * data "from-b"]
        set id3 [$client3 XADD mystream IDMP service-c "order-123" * data "from-c"]
        
        # All should be different entries
        assert {$id1 ne $id2}
        assert {$id2 ne $id3}
        assert_equal 3 [r XLEN mystream]
        
        # Duplicate from same service should return same ID
        set id1_dup [$client1 XADD mystream IDMP service-a "order-123" * data "retry"]
        assert_equal $id1 $id1_dup
        
        # Verify pids-tracked
        set reply [r XINFO STREAM mystream]
        assert_equal 3 [dict get $reply pids-tracked]
        
        # Cleanup
        $client1 close
        $client2 close
        $client3 close
    }

    test {XADD IDMP multiple producers pipelined requests} {
        r DEL mystream
        
        # Send pipelined requests from multiple producers
        r MULTI
        r XADD mystream IDMP p1 "req-1" * field "v1"
        r XADD mystream IDMP p2 "req-1" * field "v2"
        r XADD mystream IDMP p1 "req-1" * field "dup"
        r XADD mystream IDMP p2 "req-2" * field "v3"
        r XADD mystream IDMP p3 "req-1" * field "v4"
        set results [r EXEC]
        
        set id_p1_r1 [lindex $results 0]
        set id_p2_r1 [lindex $results 1]
        set id_p1_r1_dup [lindex $results 2]
        set id_p2_r2 [lindex $results 3]
        set id_p3_r1 [lindex $results 4]
        
        # p1 req-1 and its duplicate should match
        assert_equal $id_p1_r1 $id_p1_r1_dup
        
        # Different producers or different IIDs should be different
        assert {$id_p1_r1 ne $id_p2_r1}
        assert {$id_p2_r1 ne $id_p2_r2}
        assert {$id_p2_r1 ne $id_p3_r1}
        
        # 4 unique entries: p1/req-1, p2/req-1, p2/req-2, p3/req-1
        assert_equal 4 [r XLEN mystream]
        
        set reply [r XINFO STREAM mystream]
        assert_equal 3 [dict get $reply pids-tracked]
    }

    test {XADD IDMP multiple producers with mixed IDMP and IDMPAUTO} {
        r DEL mystream
        
        # Mix of IDMP and IDMPAUTO from different producers
        set id1 [r XADD mystream IDMP p1 "explicit-iid" * field "v1"]
        set id2 [r XADD mystream IDMPAUTO p2 * field "v1"]
        set id3 [r XADD mystream IDMP p3 "another-iid" * field "v1"]
        set id4 [r XADD mystream IDMPAUTO p4 * field "v1"]
        
        # All should be different entries
        assert {$id1 ne $id2}
        assert {$id2 ne $id3}
        assert {$id3 ne $id4}
        assert_equal 4 [r XLEN mystream]
        
        # Duplicates should work for each type
        set id1_dup [r XADD mystream IDMP p1 "explicit-iid" * field "dup"]
        set id2_dup [r XADD mystream IDMPAUTO p2 * field "v1"]
        
        assert_equal $id1 $id1_dup
        assert_equal $id2 $id2_dup
        
        set reply [r XINFO STREAM mystream]
        assert_equal 4 [dict get $reply pids-tracked]
    }

    test {XADD IDMP multiple producers stress test} {
        r DEL mystream
        
        # Create many producers
        set num_producers 50
        set ids {}
        
        for {set i 0} {$i < $num_producers} {incr i} {
            lappend ids [r XADD mystream IDMP "producer-$i" "request-1" * field "value-$i"]
        }
        
        # Verify all entries exist
        assert_equal $num_producers [r XLEN mystream]
        
        # Verify pids-tracked
        set reply [r XINFO STREAM mystream]
        assert_equal $num_producers [dict get $reply pids-tracked]
        assert_equal $num_producers [dict get $reply iids-tracked]
        
        # Verify deduplication for each producer
        for {set i 0} {$i < $num_producers} {incr i} {
            set original_id [lindex $ids $i]
            set dup_id [r XADD mystream IDMP "producer-$i" "request-1" * field "dup"]
            assert_equal $original_id $dup_id
        }
        
        # No new entries should have been added
        assert_equal $num_producers [r XLEN mystream]
    }

    test {XADD IDMPAUTO with invalid syntax} {
        r DEL mystream
        assert_error "*IDMP/IDMPAUTO specified multiple times*" {r XADD mystream IDMPAUTO p1 IDMPAUTO p2 * f v}
        assert_error "*IDMP/IDMPAUTO specified multiple times*" {r XADD mystream IDMPAUTO p1 IDMP p2 iid1 * f v}
        assert_error "*IDMP/IDMPAUTO specified multiple times*" {r XADD mystream IDMP p1 iid1 IDMPAUTO p2 * f v}
        assert_error "*IDMP/IDMPAUTO can be used only with auto-generated IDs*" {r XADD mystream IDMPAUTO p1 1-1 f v}
    }

    test {XADD IDMPAUTO basic deduplication based on field-value pairs} {
        r DEL mystream
        
        # First XADD with IDMPAUTO
        set id1 [r XADD mystream IDMPAUTO p1 * amount "100" currency "USD"]
        assert {[regexp {^[0-9]+-[0-9]+$} $id1]}
        
        # Second XADD with same fields and values should deduplicate
        set id2 [r XADD mystream IDMPAUTO p1 * amount "100" currency "USD"]
        assert_equal $id1 $id2
        
        # Verify only one entry exists
        assert_equal 1 [r XLEN mystream]
        
        # Third XADD with different values should create new entry
        set id3 [r XADD mystream IDMPAUTO p1 * amount "200" currency "USD"]
        assert {$id3 != $id1}
        assert_equal 2 [r XLEN mystream]
    }

    test {XADD IDMPAUTO deduplicates regardless of field order} {
        r DEL mystream
        
        # Add entry with fields in one order
        set id1 [r XADD mystream IDMPAUTO p1 * field1 "a" field2 "b" field3 "c"]
        
        # Add entry with same fields in different order (should deduplicate)
        set id2 [r XADD mystream IDMPAUTO p1 * field2 "b" field3 "c" field1 "a"]
        assert_equal $id1 $id2
        
        # Verify only one entry exists
        assert_equal 1 [r XLEN mystream]
        
        # Add entry with different order but different values (should be new)
        set id3 [r XADD mystream IDMPAUTO p1 * field3 "c" field1 "x" field2 "b"]
        assert {$id3 != $id1}
        assert_equal 2 [r XLEN mystream]
    }

    test {XADD IDMPAUTO different field-value pairs create different entries} {
        r DEL mystream
        
        # Add different entries
        set id1 [r XADD mystream IDMPAUTO p1 * user "alice" action "login"]
        set id2 [r XADD mystream IDMPAUTO p1 * user "bob" action "login"]
        set id3 [r XADD mystream IDMPAUTO p1 * user "alice" action "logout"]
        
        # Verify all IDs are different
        assert {$id1 != $id2}
        assert {$id2 != $id3}
        assert {$id1 != $id3}
        
        # Verify all entries exist
        assert_equal 3 [r XLEN mystream]
    }

    test {XADD IDMPAUTO with single field-value pair} {
        r DEL mystream
        
        # Add entry with single field
        set id1 [r XADD mystream IDMPAUTO p1 * status "active"]
        set id2 [r XADD mystream IDMPAUTO p1 * status "active"]
        assert_equal $id1 $id2
        
        # Different value should create new entry
        set id3 [r XADD mystream IDMPAUTO p1 * status "inactive"]
        assert {$id3 != $id1}
        assert_equal 2 [r XLEN mystream]
    }

    test {XADD IDMPAUTO with many field-value pairs} {
        r DEL mystream
        
        # Add entry with many fields
        set id1 [r XADD mystream IDMPAUTO p1 * f1 "v1" f2 "v2" f3 "v3" f4 "v4" f5 "v5" f6 "v6" f7 "v7" f8 "v8"]
        set id2 [r XADD mystream IDMPAUTO p1 * f1 "v1" f2 "v2" f3 "v3" f4 "v4" f5 "v5" f6 "v6" f7 "v7" f8 "v8"]
        assert_equal $id1 $id2
        
        # Change one value should create new entry
        set id3 [r XADD mystream IDMPAUTO p1 * f1 "v1" f2 "v2" f3 "v3" f4 "v4" f5 "v5" f6 "v6" f7 "v7" f8 "different"]
        assert {$id3 != $id1}
        assert_equal 2 [r XLEN mystream]
    }

    test {XADD IDMPAUTO with binary-safe values} {
        r DEL mystream
        
        # Test with null bytes and binary data
        set binary_val "\x00\x01\x02\xff"
        set id1 [r XADD mystream IDMPAUTO p1 * field $binary_val]
        set id2 [r XADD mystream IDMPAUTO p1 * field $binary_val]
        assert_equal $id1 $id2
        assert_equal 1 [r XLEN mystream]
    }

    test {XADD IDMPAUTO with unicode values} {
        r DEL mystream
        
        # Test with unicode characters
        set id1 [r XADD mystream IDMPAUTO p1 * message "hello世界"]
        set id2 [r XADD mystream IDMPAUTO p1 * message "hello世界"]
        assert_equal $id1 $id2
        
        # Different unicode should create new entry
        set id3 [r XADD mystream IDMPAUTO p1 * message "héllo"]
        assert {$id3 != $id1}
        assert_equal 2 [r XLEN mystream]
    }

    test {XADD IDMPAUTO with long values} {
        r DEL mystream
        
        # Test with very long values
        set long_val [string repeat "x" 10000]
        set id1 [r XADD mystream IDMPAUTO p1 * data $long_val]
        set id2 [r XADD mystream IDMPAUTO p1 * data $long_val]
        assert_equal $id1 $id2
        assert_equal 1 [r XLEN mystream]
    }

    test {XADD IDMPAUTO with empty string values} {
        r DEL mystream
        
        # Test with empty string values
        set id1 [r XADD mystream IDMPAUTO p1 * field ""]
        set id2 [r XADD mystream IDMPAUTO p1 * field ""]
        assert_equal $id1 $id2
        
        # Non-empty should be different
        set id3 [r XADD mystream IDMPAUTO p1 * field "value"]
        assert {$id3 != $id1}
        assert_equal 2 [r XLEN mystream]
    }

    test {XADD IDMPAUTO with MAXLEN option} {
        r DEL mystream
        
        # Add entries with IDMPAUTO and MAXLEN
        set id1 [r XADD mystream IDMPAUTO p1 MAXLEN ~ 100 * field "value1"]
        set id2 [r XADD mystream IDMPAUTO p1 MAXLEN ~ 100 * field "value2"]
        
        # Attempt duplicate
        set id1_dup [r XADD mystream IDMPAUTO p1 MAXLEN ~ 100 * field "value1"]
        
        # Verify deduplication works
        assert_equal $id1 $id1_dup
        
        # Verify only 2 entries exist
        assert_equal 2 [r XLEN mystream]
    }

    test {XADD IDMPAUTO with MINID option} {
        r DEL mystream
        
        # Add entry with IDMPAUTO and MINID
        set id1 [r XADD mystream IDMPAUTO p1 MINID ~ 1000000000-0 * field "value1"]
        
        # Attempt duplicate with MINID
        set id2 [r XADD mystream IDMPAUTO p1 MINID ~ 1000000000-0 * field "value1"]
        
        # Verify deduplication works
        assert_equal $id1 $id2
        assert_equal 1 [r XLEN mystream]
    }

    test {XADD IDMPAUTO with NOMKSTREAM option} {
        r DEL mystream
        
        # Attempt XADD with NOMKSTREAM on non-existent stream
        set result [r XADD mystream NOMKSTREAM IDMPAUTO p1 * field "value"]
        assert_equal {} $result
        
        # Create stream first
        r XADD mystream * field "initial"
        
        # Now NOMKSTREAM with IDMPAUTO should work
        set id [r XADD mystream NOMKSTREAM IDMPAUTO p1 * field "test"]
        assert {[regexp {^[0-9]+-[0-9]+$} $id]}
    }

    test {XADD IDMPAUTO with KEEPREF option} {
        r DEL mystream
        
        # Add entries with IDMPAUTO and KEEPREF
        set id1 [r XADD mystream KEEPREF IDMPAUTO p1 * field "value1"]
        set id2 [r XADD mystream KEEPREF IDMPAUTO p1 * field "value1"]
        
        # Verify deduplication works with KEEPREF
        assert_equal $id1 $id2
        assert_equal 1 [r XLEN mystream]
    }

    test {XADD IDMPAUTO argument order variations} {
        r DEL mystream
        
        # Test different argument orders
        set id1 [r XADD mystream IDMPAUTO p1 * field "test"]
        set id2 [r XADD mystream IDMPAUTO p1 MAXLEN ~ 100 * field "test2"]
        set id3 [r XADD mystream MAXLEN ~ 100 IDMPAUTO p1 * field "test3"]
        set id4 [r XADD mystream KEEPREF IDMPAUTO p1 * field "test4"]
        set id5 [r XADD mystream IDMPAUTO p1 KEEPREF * field "test5"]
        
        # All should be valid stream IDs
        assert {[regexp {^[0-9]+-[0-9]+$} $id1]}
        assert {[regexp {^[0-9]+-[0-9]+$} $id2]}
        assert {[regexp {^[0-9]+-[0-9]+$} $id3]}
        assert {[regexp {^[0-9]+-[0-9]+$} $id4]}
        assert {[regexp {^[0-9]+-[0-9]+$} $id5]}
        
        # Verify all entries exist
        assert_equal 5 [r XLEN mystream]
    }

    test {XADD IDMPAUTO persists in RDB} {
        r DEL mystream
        
        # Add entries with IDMPAUTO
        set id1 [r XADD mystream IDMPAUTO p1 * field "value1"]
        set id2 [r XADD mystream IDMPAUTO p1 * field "value2"]
        
        # Save and reload
        r DEBUG RELOAD
        
        # Verify stream exists
        assert_equal 2 [r XLEN mystream]
        
        # Verify deduplication still works after restart
        set id1_dup [r XADD mystream IDMPAUTO p1 * field "value1"]
        assert_equal $id1 $id1_dup
        
        # Should still have only 2 entries
        assert_equal 2 [r XLEN mystream]
    } {} {external:skip needs:debug}

    test {XADD IDMPAUTO with consumer groups} {
        r DEL mystream
        
        # Create consumer group
        r XADD mystream * initial "value"
        r XGROUP CREATE mystream mygroup 0
        
        # Add entries with IDMPAUTO
        set id1 [r XADD mystream IDMPAUTO p1 * event "login" user "alice"]
        set id2 [r XADD mystream IDMPAUTO p1 * event "logout" user "bob"]
        
        # Attempt duplicate
        set id1_dup [r XADD mystream IDMPAUTO p1 * event "login" user "alice"]
        assert_equal $id1 $id1_dup
        
        # Read from consumer group (should get 3 new entries, not 4)
        set messages [r XREADGROUP GROUP mygroup consumer1 COUNT 10 STREAMS mystream >]
        set stream_data [lindex $messages 0 1]
        assert_equal 3 [llength $stream_data]
    }

    test {XADD IDMPAUTO field names matter} {
        r DEL mystream
        
        # Different field names should create different entries
        set id1 [r XADD mystream IDMPAUTO p1 * field1 "value"]
        set id2 [r XADD mystream IDMPAUTO p1 * field2 "value"]
        
        assert {$id1 != $id2}
        assert_equal 2 [r XLEN mystream]
    }

    test {XADD IDMPAUTO with numeric field names and values} {
        r DEL mystream
        
        # Test with numeric field names
        set id1 [r XADD mystream IDMPAUTO p1 * 123 "456" 789 "012"]
        set id2 [r XADD mystream IDMPAUTO p1 * 123 "456" 789 "012"]
        assert_equal $id1 $id2
        
        # Different numeric values
        set id3 [r XADD mystream IDMPAUTO p1 * 123 "999" 789 "012"]
        assert {$id3 != $id1}
        assert_equal 2 [r XLEN mystream]
    }

    test {XADD IDMPAUTO multiple producers have isolated namespaces} {
        r DEL mystream
        
        # Same field-value pairs with different producers should create separate entries
        set id1 [r XADD mystream IDMPAUTO producer1 * amount "100" currency "USD"]
        set id2 [r XADD mystream IDMPAUTO producer2 * amount "100" currency "USD"]
        
        # Different producers = different entries
        assert {$id1 ne $id2}
        assert_equal 2 [r XLEN mystream]
        
        # Same producer with same fields should deduplicate
        set id1_dup [r XADD mystream IDMPAUTO producer1 * amount "100" currency "USD"]
        set id2_dup [r XADD mystream IDMPAUTO producer2 * amount "100" currency "USD"]
        
        assert_equal $id1 $id1_dup
        assert_equal $id2 $id2_dup
        assert_equal 2 [r XLEN mystream]
    }

    test {XADD IDMPAUTO multiple producers} {
        r DEL mystream
        
        # Different producers with same content should create separate entries
        set id1 [r XADD mystream IDMPAUTO app1 * event "login" user "alice"]
        set id2 [r XADD mystream IDMPAUTO app2 * event "login" user "alice"]
        set id3 [r XADD mystream IDMPAUTO app3 * event "login" user "alice"]
        
        # All should be different (different producers)
        assert {$id1 ne $id2}
        assert {$id2 ne $id3}
        assert_equal 3 [r XLEN mystream]
        
        # Same producer with same content should deduplicate
        set id1_dup [r XADD mystream IDMPAUTO app1 * event "login" user "alice"]
        assert_equal $id1 $id1_dup
        
        set reply [r XINFO STREAM mystream]
        assert_equal 3 [dict get $reply pids-tracked]
    }

    test {XIDMP entries expire after DURATION seconds} {
        r DEL mystream
        r XADD mystream IDMP p1 "req-1" * field "value1"
        r XCFGSET mystream IDMP-DURATION 1
        
        # Immediate duplicate should be detected
        set id1 [r XADD mystream IDMP p1 "req-1" * field "value1"]
        set id2 [r XADD mystream IDMP p1 "req-1" * field "value2"]
        assert_equal $id1 $id2
        
        # Wait for expiration (1 second + margin)
        after 2500
        
        # Now should create new entry
        set id3 [r XADD mystream IDMP p1 "req-1" * field "value3"]
        assert {$id1 ne $id3}
    }

    test {XIDMP set evicts entries when MAXSIZE is reached} {
        r DEL mystream
        
        # First add an entry to create the stream, then set config
        r XADD mystream IDMP p1 "init" * field "init"
        r XCFGSET mystream IDMP-MAXSIZE 3 IDMP-DURATION 60
        
        # Add 3 unique entries
        set id1 [r XADD mystream IDMP p1 "req-1" * field "v1"]
        set id2 [r XADD mystream IDMP p1 "req-2" * field "v2"]
        set id3 [r XADD mystream IDMP p1 "req-3" * field "v3"]
        
        # All duplicates should still work (IDMP set has: req-1, req-2, req-3)
        assert_equal $id1 [r XADD mystream IDMP p1 "req-1" * field "dup"]
        assert_equal $id2 [r XADD mystream IDMP p1 "req-2" * field "dup"]
        assert_equal $id3 [r XADD mystream IDMP p1 "req-3" * field "dup"]
        
        # Add 4th entry - should evict oldest (req-1)
        set id4 [r XADD mystream IDMP p1 "req-4" * field "v4"]
        
        # req-1 should be evicted, so it should create new entry
        set result [r XADD mystream IDMP p1 "req-1" * field "new"]
        assert {$result ne $id1}
        
        # req-2 is also eveicted but req-3 should still be in the set
        assert_equal $id3 [r XADD mystream IDMP p1 "req-3" * field "dup2"]
        
        # Stream should have: init, req-1, req-2, req-3, req-4, req-1(new) = 6 entries
        assert_equal 6 [r XLEN mystream]
    }

    test {XCFGSET set IDMP-DURATION successfully} {
        r DEL mystream
        
        # Create stream with IDMP entry
        r XADD mystream IDMP p1 "req-1" * field "value"
        
        # Set IDMP-DURATION to 5s
        assert_equal "OK" [r XCFGSET mystream IDMP-DURATION 5]
        
        # Verify IDMP-DURATION was set
        set reply [r XINFO STREAM mystream]
        assert_equal 5 [dict get $reply idmp-duration]
    }

    test {XCFGSET set IDMP-MAXSIZE successfully} {
        r DEL mystream
        
        # Create stream with IDMP entry
        r XADD mystream IDMP p1 "req-1" * field "value"
        
        # Set IDMP-MAXSIZE to 5000
        assert_equal "OK" [r XCFGSET mystream IDMP-MAXSIZE 5000]
        
        # Verify IDMP-MAXSIZE was set
        set reply [r XINFO STREAM mystream]
        assert_equal 5000 [dict get $reply idmp-maxsize]
    }

    test {XCFGSET set both IDMP-DURATION and IDMP-MAXSIZE} {
        r DEL mystream
        
        # Create stream with IDMP entry
        r XADD mystream IDMP p1 "req-1" * field "value"
        
        # Set both IDMP-DURATION and IDMP-MAXSIZE
        assert_equal "OK" [r XCFGSET mystream IDMP-DURATION 3 IDMP-MAXSIZE 10000]
        
        # Verify both were set
        set reply [r XINFO STREAM mystream]
        assert_equal 3 [dict get $reply idmp-duration]
        assert_equal 10000 [dict get $reply idmp-maxsize]
    }

    test {XINFO STREAM shows IDMP configuration parameters} {
        r DEL mystream
        
        # Create stream with IDMP entry
        r XADD mystream IDMP p1 "req-1" * field "value"
        
        # Set both IDMP-DURATION and IDMP-MAXSIZE
        assert_equal "OK" [r XCFGSET mystream IDMP-DURATION 3 IDMP-MAXSIZE 10000]
        
        # Verify both were set
        set reply [r XINFO STREAM mystream]
        assert_equal 3 [dict get $reply idmp-duration]
        assert_equal 10000 [dict get $reply idmp-maxsize]
    }

    test {XINFO STREAM shows default IDMP parameters} {
        r DEL mystream
        
        # Create stream with IDMP entry
        r XADD mystream IDMP p1 "req-1" * field "value"
        
        # Verify default parameters
        set reply [r XINFO STREAM mystream]
        assert_equal 100 [dict get $reply idmp-duration]
        assert_equal 100 [dict get $reply idmp-maxsize]
    }

    test {XCFGSET error on non-existent stream} {
        r DEL mystream
        
        # Attempt to set config on non-existent stream
        assert_error "*no such key*" {r XCFGSET mystream IDMP-DURATION 5}
    }

    test {XCFGSET IDMP-DURATION maximum value validation} {
        r DEL mystream
        
        # Create stream with IDMP
        r XADD mystream IDMP p1 "req-1" * field "value"
        
        # Set IDMP-DURATION to maximum allowed (86400 seconds = 24 hours)
        assert_equal "OK" [r XCFGSET mystream IDMP-DURATION 86400]
        
        # Verify it was set
        set reply [r XINFO STREAM mystream]
        assert_equal 86400 [dict get $reply idmp-duration]
        
        # Attempt to set IDMP-DURATION above maximum
        assert_error "*ERR IDMP-DURATION must be*" {r XCFGSET mystream IDMP-DURATION 86401}
        
        # Verify IDMP-DURATION wasn't changed
        set reply [r XINFO STREAM mystream]
        assert_equal 86400 [dict get $reply idmp-duration]
    }

    test {XCFGSET IDMP-DURATION minimum value validation} {
        r DEL mystream
        
        # Create stream with IDMP
        r XADD mystream IDMP p1 "req-1" * field "value"
        
        # Attempt to set IDMP-DURATION to 0
        assert_error "*ERR IDMP-DURATION must be between*" {r XCFGSET mystream IDMP-DURATION 0}
        
        # Attempt to set IDMP-DURATION to negative value
        assert_error "*ERR IDMP-DURATION must be between*" {r XCFGSET mystream IDMP-DURATION -100}
        
        # Set IDMP-DURATION to minimum valid value (1 second)
        assert_equal "OK" [r XCFGSET mystream IDMP-DURATION 1]
        
        # Verify it was set
        set reply [r XINFO STREAM mystream]
        assert_equal 1 [dict get $reply idmp-duration]
    }

    test {XCFGSET IDMP-MAXSIZE maximum value validation} {
        r DEL mystream
        
        # Create stream with IDMP
        r XADD mystream IDMP p1 "req-1" * field "value"
        
        # Set IDMP-MAXSIZE to maximum allowed (10000)
        assert_equal "OK" [r XCFGSET mystream IDMP-MAXSIZE 10000]
        
        # Verify it was set
        set reply [r XINFO STREAM mystream]
        assert_equal 10000 [dict get $reply idmp-maxsize]
        
        # Attempt to set IDMP-MAXSIZE above maximum
        assert_error "*ERR IDMP-MAXSIZE must be between*" {r XCFGSET mystream IDMP-MAXSIZE 10001}
        
        # Verify IDMP-MAXSIZE wasn't changed
        set reply [r XINFO STREAM mystream]
        assert_equal 10000 [dict get $reply idmp-maxsize]
    }

    test {XCFGSET IDMP-MAXSIZE minimum value validation} {
        r DEL mystream
        
        # Create stream with IDMP
        r XADD mystream IDMP p1 "req-1" * field "value"
        
        # Attempt to set IDMP-MAXSIZE to 0
        assert_error "*ERR IDMP-MAXSIZE must be between*" {r XCFGSET mystream IDMP-MAXSIZE 0}
        
        # Attempt to set IDMP-MAXSIZE to negative value
        assert_error "*ERR IDMP-MAXSIZE must be between*" {r XCFGSET mystream IDMP-MAXSIZE -50}
        
        # Set IDMP-MAXSIZE to minimum valid value (1)
        assert_equal "OK" [r XCFGSET mystream IDMP-MAXSIZE 1]
        
        # Verify it was set
        set reply [r XINFO STREAM mystream]
        assert_equal 1 [dict get $reply idmp-maxsize]
    }

    test {XCFGSET invalid syntax} {
        r DEL mystream
        
        # Create stream with IDMP
        r XADD mystream IDMP p1 "req-1" * field "value"
        
        # Attempt CFGSET with invalid syntax
        assert_error "*ERR At least one parameter*" {r XCFGSET mystream}
        assert_error "*syntax*" {r XCFGSET mystream IDMP-DURATION}
        assert_error "*syntax*" {r XCFGSET mystream IDMP-MAXSIZE}
        assert_error "*ERR value is not an integer*" {r XCFGSET mystream IDMP-DURATION A}
        assert_error "*ERR value is not an integer*" {r XCFGSET mystream IDMP-DURATION AAA}
        assert_error "*ERR value is not an integer*" {r XCFGSET mystream IDMP-DURATION *}
        assert_error "*ERR value is not an integer*" {r XCFGSET mystream IDMP-DURATION -}
        assert_error "*ERR value is not an integer*" {r XCFGSET mystream IDMP-DURATION +}
        assert_error "*ERR value is not an integer*" {r XCFGSET mystream IDMP-DURATION 120-5}
        assert_error "*ERR value is not an integer*" {r XCFGSET mystream IDMP-DURATION 3.14}
        assert_error "*ERR value is not an integer*" {r XCFGSET mystream IDMP-DURATION 000000000}
        assert_error "*ERR value is not an integer*" {r XCFGSET mystream IDMP-DURATION IDMP-DURATION}
        assert_error "*ERR value is not an integer*" {r XCFGSET mystream IDMP-DURATION IDMP-DURATION IDMP-DURATION}
        assert_error "*ERR value is not an integer*" {r XCFGSET mystream IDMP-MAXSIZE A}
        assert_error "*ERR value is not an integer*" {r XCFGSET mystream IDMP-MAXSIZE AAA}
        assert_error "*ERR value is not an integer*" {r XCFGSET mystream IDMP-MAXSIZE *}
        assert_error "*ERR value is not an integer*" {r XCFGSET mystream IDMP-MAXSIZE -}
        assert_error "*ERR value is not an integer*" {r XCFGSET mystream IDMP-MAXSIZE +}
        assert_error "*ERR value is not an integer*" {r XCFGSET mystream IDMP-MAXSIZE 120-5}
        assert_error "*ERR value is not an integer*" {r XCFGSET mystream IDMP-MAXSIZE 3.14}
        assert_error "*ERR value is not an integer*" {r XCFGSET mystream IDMP-MAXSIZE 000000000}
        assert_error "*ERR value is not an integer*" {r XCFGSET mystream IDMP-MAXSIZE IDMP-MAXSIZE}
        assert_error "*ERR value is not an integer*" {r XCFGSET mystream IDMP-MAXSIZE IDMP-MAXSIZE IDMP-MAXSIZE}

        assert_error "*syntax*" {r XCFGSET mystream INVALID}
        assert_error "*ERR value is not an integer*" {r XCFGSET mystream IDMP-DURATION INVALID IDMP-MAXSIZE}
        assert_error "*ERR value is not an integer*" {r XCFGSET mystream IDMP-MAXSIZE INVALID IDMP-DURATION}
    }

    test {XCFGSET multiple configuration changes} {
        r DEL mystream
        
        # Create stream with IDMP
        r XADD mystream IDMP p1 "req-1" * field "value"
        
        # Change DURATION multiple times
        r XCFGSET mystream IDMP-DURATION 1
        r XCFGSET mystream IDMP-DURATION 2
        r XCFGSET mystream IDMP-DURATION 3
        
        # Change MAXSIZE
        r XCFGSET mystream IDMP-MAXSIZE 100
        r XCFGSET mystream IDMP-MAXSIZE 200
        
        # Verify latest values
        set reply [r XINFO STREAM mystream]
        assert_equal 3 [dict get $reply idmp-duration]
        assert_equal 200 [dict get $reply idmp-maxsize]
    }

    test {XCFGSET configuration persists in RDB} {
        r DEL mystream
        
        # Create stream and set configuration
        r XADD mystream IDMP p1 "req-1" * field "value"
        r XCFGSET mystream IDMP-DURATION 75 IDMP-MAXSIZE 7500
        
        # Save and restart
        r SAVE

        # Restart Redis
        restart_server 0 true false
        
        # Verify configuration persisted
        set reply [r XINFO STREAM mystream]
        assert_equal 75 [dict get $reply idmp-duration]
        assert_equal 7500 [dict get $reply idmp-maxsize]
    } {} {external:skip}

    test {XCFGSET configuration in AOF} {
        r DEL mystream
        r config set appendonly yes
        
        # Wait for the automatic AOF rewrite triggered by enabling AOF
        waitForBgrewriteaof r

        # Create stream and set configuration
        r XADD mystream IDMP p1 "req-1" * field "value"
        r XCFGSET mystream IDMP-DURATION 45 IDMP-MAXSIZE 4500
        
        # Force AOF rewrite
        r BGREWRITEAOF
        waitForBgrewriteaof r
        
        # Restart with AOF
        r DEBUG RELOAD
        
        # Verify configuration
        set reply [r XINFO STREAM mystream]
        assert_equal 45 [dict get $reply idmp-duration]
        assert_equal 4500 [dict get $reply idmp-maxsize]
        
        assert_equal "OK" [r config set appendonly no]
    } {} {external:skip needs:debug}

    test {XCFGSET changing IDMP-DURATION clears all iids history} {
        r DEL mystream
        
        # Create stream and add entries with IDMP
        set id1 [r XADD mystream IDMP p1 "req-1" * field "value1"]
        set id2 [r XADD mystream IDMP p1 "req-2" * field "value2"]
        
        # Verify deduplication works before config change
        set dup_id [r XADD mystream IDMP p1 "req-1" * field "dup"]
        assert_equal $id1 $dup_id
        
        # Change DURATION - should clear iids history
        r XCFGSET mystream IDMP-DURATION 5
        
        # Now req-1 should create a new entry (history was cleared)
        set new_id1 [r XADD mystream IDMP p1 "req-1" * field "new1"]
        assert {$id1 ne $new_id1}
        
        # Should have 3 entries total (2 original + 1 new)
        assert_equal 3 [r XLEN mystream]
    }

    test {XCFGSET changing IDMP-MAXSIZE clears all iids history} {
        r DEL mystream
        
        # Create stream and add entries with IDMP
        set id1 [r XADD mystream IDMP p1 "req-1" * field "value1"]
        set id2 [r XADD mystream IDMP p1 "req-2" * field "value2"]
        
        # Verify deduplication works before config change
        set dup_id [r XADD mystream IDMP p1 "req-2" * field "dup"]
        assert_equal $id2 $dup_id
        
        # Change MAXSIZE - should clear iids history
        r XCFGSET mystream IDMP-MAXSIZE 5000
        
        # Now req-2 should create a new entry (history was cleared)
        set new_id2 [r XADD mystream IDMP p1 "req-2" * field "new2"]
        assert {$id2 ne $new_id2}
        
        # Should have 3 entries total (2 original + 1 new)
        assert_equal 3 [r XLEN mystream]
    }

    test {XCFGSET history cleared then new deduplication works} {
        r DEL mystream
        
        # Create stream and add entries
        set id1 [r XADD mystream IDMP p1 "req-1" * field "value1"]
        
        # Change configuration to clear history
        r XCFGSET mystream IDMP-DURATION 6
        
        # Add new entry with same iid
        set new_id1 [r XADD mystream IDMP p1 "req-1" * field "new1"]
        assert {$id1 ne $new_id1}
        
        # Now deduplication should work with new history
        set dup_id1 [r XADD mystream IDMP p1 "req-1" * field "dup1"]
        assert_equal $new_id1 $dup_id1
        
        # Should have 2 entries (1 original + 1 new)
        assert_equal 2 [r XLEN mystream]
    }

    test {XCFGSET history cleared preserves stream entries} {
        r DEL mystream
        
        # Create stream with entries
        set id1 [r XADD mystream IDMP p1 "req-1" * field "value1" data "data1"]
        set id2 [r XADD mystream IDMP p1 "req-2" * field "value2" data "data2"]
        
        # Verify entries exist with correct data
        set entries [r XRANGE mystream - +]
        assert_equal 2 [llength $entries]
        
        # Change configuration to clear iids history
        r XCFGSET mystream IDMP-DURATION 7
        
        # Stream entries should still exist unchanged
        set entries_after [r XRANGE mystream - +]
        assert_equal 2 [llength $entries_after]
        
        # Verify original entries have correct data
        set entry1_fields [lindex [lindex $entries_after 0] 1]
        assert_equal "value1" [dict get $entry1_fields field]
        assert_equal "data1" [dict get $entry1_fields data]
        
        # But iids history is cleared, so can add new entries
        set new_id1 [r XADD mystream IDMP p1 "req-1" * field "new1"]
        assert {$id1 ne $new_id1}
    }

    test {XCFGSET setting same IDMP-DURATION does not clear iids history} {
        r DEL mystream
        
        # Create stream and add entries with IDMP
        set id1 [r XADD mystream IDMP p1 "req-1" * field "value1"]
        set id2 [r XADD mystream IDMP p1 "req-2" * field "value2"]
        
        # Verify deduplication works before config
        set dup_id [r XADD mystream IDMP p1 "req-1" * field "dup"]
        assert_equal $id1 $dup_id
        
        # Get current DURATION (default is 100)
        set reply [r XINFO STREAM mystream]
        set current_duration [dict get $reply idmp-duration]
        assert_equal 100 $current_duration
        
        # Set IDMP-DURATION to same value - should NOT clear iids history
        r XCFGSET mystream IDMP-DURATION 100
        
        # Deduplication should still work (history was NOT cleared)
        set dup_id2 [r XADD mystream IDMP p1 "req-1" * field "dup2"]
        assert_equal $id1 $dup_id2
        
        set dup_id3 [r XADD mystream IDMP p1 "req-2" * field "dup3"]
        assert_equal $id2 $dup_id3
        
        # Should still have 2 entries (no new entries added)
        assert_equal 2 [r XLEN mystream]
    }

    test {XCFGSET setting same IDMP-MAXSIZE does not clear iids history} {
        r DEL mystream
        
        # Create stream and add entries with IDMP
        set id1 [r XADD mystream IDMP p1 "req-1" * field "value1"]
        set id2 [r XADD mystream IDMP p1 "req-2" * field "value2"]
        
        # Verify deduplication works
        set dup_id [r XADD mystream IDMP p1 "req-2" * field "dup"]
        assert_equal $id2 $dup_id
        
        # Get current MAXSIZE (default is 100)
        set reply [r XINFO STREAM mystream]
        set current_maxsize [dict get $reply idmp-maxsize]
        assert_equal 100 $current_maxsize
        
        # Set IDMP-MAXSIZE to same value - should NOT clear iids history
        r XCFGSET mystream IDMP-MAXSIZE 100
        
        # Deduplication should still work (history was NOT cleared)
        set dup_id2 [r XADD mystream IDMP p1 "req-1" * field "dup2"]
        assert_equal $id1 $dup_id2
        
        set dup_id3 [r XADD mystream IDMP p1 "req-2" * field "dup3"]
        assert_equal $id2 $dup_id3
        
        # Should still have 2 entries (no new entries added)
        assert_equal 2 [r XLEN mystream]
    }

    test {XCFGSET repeated same-value calls preserve IDMP history} {
        r DEL mystream
        
        # Set configuration first
        r XADD mystream * field "init"
        r XCFGSET mystream IDMP-DURATION 10 IDMP-MAXSIZE 5000
        
        # Create stream with initial entry after config is set
        set id1 [r XADD mystream IDMP p1 "req-1" * field "value1"]
        
        # Call XCFGSET multiple times with same values
        # (common pattern for configuration initialization)
        r XCFGSET mystream IDMP-DURATION 10 IDMP-MAXSIZE 5000
        r XCFGSET mystream IDMP-DURATION 10 IDMP-MAXSIZE 5000
        r XCFGSET mystream IDMP-DURATION 10 IDMP-MAXSIZE 5000
        
        # Deduplication should still work (history not cleared by same-value sets)
        set dup_id [r XADD mystream IDMP p1 "req-1" * field "dup"]
        assert_equal $id1 $dup_id
        
        # Add new entry from second producer
        set id2 [r XADD mystream IDMP p2 "req-2" * field "value2"]
        
        # Both producers should work with deduplication
        set dup_id2 [r XADD mystream IDMP p2 "req-2" * field "dup2"]
        assert_equal $id2 $dup_id2
        
        # Should have 3 entries total (init + 2 IDMP entries)
        assert_equal 3 [r XLEN mystream]
    }

    test {XCFGSET changing value after same-value sets still clears history} {
        r DEL mystream
        
        # Create stream with entries
        set id1 [r XADD mystream IDMP p1 "req-1" * field "value1"]
        
        # Set to same value multiple times (doesn't clear)
        r XCFGSET mystream IDMP-DURATION 100
        r XCFGSET mystream IDMP-DURATION 100
        
        # Verify deduplication still works
        set dup_id [r XADD mystream IDMP p1 "req-1" * field "dup"]
        assert_equal $id1 $dup_id
        
        # Now change to different value (should clear)
        r XCFGSET mystream IDMP-DURATION 50
        
        # Deduplication should not work anymore (history cleared)
        set new_id [r XADD mystream IDMP p1 "req-1" * field "new"]
        assert {$id1 ne $new_id}
        
        # Should have 2 entries now
        assert_equal 2 [r XLEN mystream]
    }

    test {XCFGSET setting same value preserves iids-tracked count} {
        r DEL mystream
        
        # Add entries with IDMP
        r XADD mystream IDMP p1 "req-1" * field "value1"
        r XADD mystream IDMP p1 "req-2" * field "value2"
        r XADD mystream IDMP p2 "req-3" * field "value3"
        
        # Verify counts
        set reply [r XINFO STREAM mystream]
        assert_equal 3 [dict get $reply iids-tracked]
        assert_equal 3 [dict get $reply iids-added]
        
        # Set to same value - should preserve counts
        r XCFGSET mystream IDMP-DURATION 100 IDMP-MAXSIZE 100
        
        set reply [r XINFO STREAM mystream]
        assert_equal 3 [dict get $reply iids-tracked]
        assert_equal 3 [dict get $reply iids-added]
    }

    test {XINFO STREAM returns iids-tracked and iids-added fields} {
        r DEL mystream
        
        # Create stream without IDMP first
        r XADD mystream * field "value"
        
        # Verify initial values: no IDMP entries yet
        set reply [r XINFO STREAM mystream]
        assert_equal 0 [dict get $reply iids-tracked]
        assert_equal 0 [dict get $reply iids-added]
        
        # Add entries with IDMP
        r XADD mystream IDMP p1 "req-1" * field "value1"
        set reply [r XINFO STREAM mystream]
        assert_equal 1 [dict get $reply iids-tracked]
        assert_equal 1 [dict get $reply iids-added]
        
        r XADD mystream IDMP p1 "req-2" * field "value2"
        set reply [r XINFO STREAM mystream]
        assert_equal 2 [dict get $reply iids-tracked]
        assert_equal 2 [dict get $reply iids-added]
        
        # Duplicate IDMP should NOT increment counters
        r XADD mystream IDMP p1 "req-1" * field "duplicate"
        set reply [r XINFO STREAM mystream]
        assert_equal 2 [dict get $reply iids-tracked]
        assert_equal 2 [dict get $reply iids-added]
        
        # Also verify FULL mode returns the same fields
        set reply_full [r XINFO STREAM mystream FULL]
        assert_equal 2 [dict get $reply_full iids-tracked]
        assert_equal 2 [dict get $reply_full iids-added]
    }

    test {XINFO STREAM iids-added is lifetime counter even after eviction} {
        r DEL mystream
        
        # Set small MAXSIZE to trigger eviction
        r XADD mystream IDMP p1 "init" * field "init"
        r XCFGSET mystream IDMP-MAXSIZE 3
        
        # Add 3 more entries (total 4, but MAXSIZE=3 so oldest evicted)
        r XADD mystream IDMP p1 "req-1" * field "v1"
        r XADD mystream IDMP p1 "req-2" * field "v2"
        r XADD mystream IDMP p1 "req-3" * field "v3"
        
        set reply [r XINFO STREAM mystream]
        # iids-tracked should be capped at MAXSIZE (3)
        assert_equal 3 [dict get $reply iids-tracked]
        # iids-added should be lifetime count (4)
        assert_equal 4 [dict get $reply iids-added]
        
        # Add more entries to verify lifetime counter keeps growing
        r XADD mystream IDMP p1 "req-4" * field "v4"
        r XADD mystream IDMP p1 "req-5" * field "v5"
        
        set reply [r XINFO STREAM mystream]
        assert_equal 3 [dict get $reply iids-tracked]
        assert_equal 6 [dict get $reply iids-added]
    }

    test {XINFO STREAM iids-duplicates is lifetime counter} {
        r DEL mystream
        
        # Add initial entry with unique IID
        r XADD mystream IDMP p1 "req-1" * field "v1"
        
        set reply [r XINFO STREAM mystream]
        # No duplicates yet
        assert_equal 0 [dict get $reply iids-duplicates]
        assert_equal 1 [dict get $reply iids-added]
        
        # Try to add duplicate IID - should be rejected and increment counter
        set dup_id [r XADD mystream IDMP p1 "req-1" * field "v1-dup"]
        
        set reply [r XINFO STREAM mystream]
        assert_equal 1 [dict get $reply iids-duplicates]
        assert_equal 1 [dict get $reply iids-added]  ;# Still 1 successful add
        
        # Try same duplicate again
        r XADD mystream IDMP p1 "req-1" * field "v1-dup2"
        
        set reply [r XINFO STREAM mystream]
        assert_equal 2 [dict get $reply iids-duplicates]
        assert_equal 1 [dict get $reply iids-added]
        
        # Add a different IID (should succeed, duplicates unchanged)
        r XADD mystream IDMP p1 "req-2" * field "v2"
        
        set reply [r XINFO STREAM mystream]
        assert_equal 2 [dict get $reply iids-duplicates]
        assert_equal 2 [dict get $reply iids-added]
        
        # Try the first IID again
        r XADD mystream IDMP p1 "req-1" * field "v1-dup3"
        
        set reply [r XINFO STREAM mystream]
        assert_equal 3 [dict get $reply iids-duplicates]
        assert_equal 2 [dict get $reply iids-added]
    }

    test {XINFO STREAM iids-duplicates persists after eviction} {
        r DEL mystream
        
        # Add initial entry and configure MAXSIZE
        r XADD mystream IDMP p1 "init" * field "init"
        r XCFGSET mystream IDMP-MAXSIZE 3
        # Note: CFGSET clears IID history, so "init" is no longer tracked
        
        # Add entries and create some duplicates
        r XADD mystream IDMP p1 "req-1" * field "v1"
        r XADD mystream IDMP p1 "req-1" * field "v1-dup"  ;# Duplicate
        r XADD mystream IDMP p1 "req-2" * field "v2"
        r XADD mystream IDMP p1 "req-2" * field "v2-dup"  ;# Duplicate
        
        set reply [r XINFO STREAM mystream]
        # iids-tracked should be 2 (req-1, req-2) - "init" was cleared by CFGSET
        assert_equal 2 [dict get $reply iids-tracked]
        # iids-added should be 3 (init, req-1, req-2) - lifetime counter includes "init"
        assert_equal 3 [dict get $reply iids-added]
        # iids-duplicates should be 2
        assert_equal 2 [dict get $reply iids-duplicates]
        
        # Add more entries to trigger eviction of old IIDs
        r XADD mystream IDMP p1 "req-3" * field "v3"
        r XADD mystream IDMP p1 "req-4" * field "v4"
        
        # Now we have: req-2, req-3, req-4 (MAXSIZE=3, so req-1 was evicted)
        
        set reply [r XINFO STREAM mystream]
        assert_equal 3 [dict get $reply iids-tracked]  ;# Now capped at MAXSIZE (3)
        assert_equal 5 [dict get $reply iids-added]    ;# 5 successful adds total
        assert_equal 2 [dict get $reply iids-duplicates]  ;# Still 2 (lifetime counter)
        
        # Try to duplicate one of the currently tracked IIDs
        r XADD mystream IDMP p1 "req-3" * field "v3-dup"
        r XADD mystream IDMP p1 "req-4" * field "v4-dup"
        
        set reply [r XINFO STREAM mystream]
        assert_equal 3 [dict get $reply iids-tracked]
        assert_equal 5 [dict get $reply iids-added]
        assert_equal 4 [dict get $reply iids-duplicates]  ;# Incremented by 2
    }

    test {XINFO STREAM iids-duplicates with multiple producers} {
        r DEL mystream
        
        # Add entries from different producers with same IID
        # (same IID but different producer = NOT a duplicate)
        r XADD mystream IDMP p1 "req-1" * field "v1-p1"
        r XADD mystream IDMP p2 "req-1" * field "v1-p2"
        
        set reply [r XINFO STREAM mystream]
        assert_equal 2 [dict get $reply pids-tracked]
        assert_equal 2 [dict get $reply iids-added]
        assert_equal 0 [dict get $reply iids-duplicates]  ;# No duplicates
        
        # Now add actual duplicates (same IID, same producer)
        r XADD mystream IDMP p1 "req-1" * field "v1-p1-dup"
        r XADD mystream IDMP p2 "req-1" * field "v1-p2-dup"
        
        set reply [r XINFO STREAM mystream]
        assert_equal 2 [dict get $reply pids-tracked]
        assert_equal 2 [dict get $reply iids-added]
        assert_equal 2 [dict get $reply iids-duplicates]  ;# 2 duplicates (one per producer)
    }

    test {XINFO STREAM iids counters after CFGSET clears history} {
        r DEL mystream
        
        # Add entries with IDMP and create some duplicates
        r XADD mystream IDMP p1 "req-1" * field "v1"
        r XADD mystream IDMP p1 "req-2" * field "v2"
        r XADD mystream IDMP p1 "req-3" * field "v3"
        r XADD mystream IDMP p1 "req-1" * field "v1-dup"  ;# Duplicate
        r XADD mystream IDMP p1 "req-2" * field "v2-dup"  ;# Duplicate
        
        set reply [r XINFO STREAM mystream]
        assert_equal 3 [dict get $reply iids-tracked]
        assert_equal 3 [dict get $reply iids-added]
        assert_equal 2 [dict get $reply iids-duplicates]
        
        # CFGSET clears IID history
        r XCFGSET mystream IDMP-DURATION 60
        
        set reply [r XINFO STREAM mystream]
        # iids-tracked should be 0 after history cleared
        assert_equal 0 [dict get $reply iids-tracked]
        # iids-added should still be preserved (lifetime counter)
        assert_equal 3 [dict get $reply iids-added]
        # iids-duplicates should still be preserved (lifetime counter)
        assert_equal 2 [dict get $reply iids-duplicates]
        
        # Add new entry and verify counters
        r XADD mystream IDMP p1 "req-4" * field "v4"
        set reply [r XINFO STREAM mystream]
        assert_equal 1 [dict get $reply iids-tracked]
        assert_equal 4 [dict get $reply iids-added]
    }

    test {XINFO STREAM iids-added persists in RDB} {
        r DEL mystream
        
        # Add entries with IDMP to build up iids-added counter
        r XADD mystream IDMP p1 "req-1" * field "v1"
        r XADD mystream IDMP p1 "req-2" * field "v2"
        r XADD mystream IDMP p1 "req-3" * field "v3"
        
        # Set small MAXSIZE to cause eviction
        r XCFGSET mystream IDMP-MAXSIZE 2
        
        # Add more to trigger eviction (iids-tracked will be 2, but iids-added=5)
        r XADD mystream IDMP p1 "req-4" * field "v4"
        r XADD mystream IDMP p1 "req-5" * field "v5"
        
        # Verify values before save
        set reply [r XINFO STREAM mystream]
        assert_equal 2 [dict get $reply iids-tracked]
        assert_equal 5 [dict get $reply iids-added]
        
        # Save and restart
        r SAVE
        restart_server 0 true false
        
        # Verify iids-added persisted after restart
        set reply [r XINFO STREAM mystream]
        assert_equal 2 [dict get $reply iids-tracked]
        assert_equal 5 [dict get $reply iids-added]
    } {} {external:skip}

    test {XINFO STREAM returns pids-tracked field} {
        r DEL mystream
        
        # Create stream without IDMP
        r XADD mystream * field "value"
        
        # Verify initial pids-tracked is 0
        set reply [r XINFO STREAM mystream]
        assert_equal 0 [dict get $reply pids-tracked]
        
        # Add entry with first producer
        r XADD mystream IDMP p1 "req-1" * field "v1"
        set reply [r XINFO STREAM mystream]
        assert_equal 1 [dict get $reply pids-tracked]
        
        # Add entry with same producer - pids-tracked should stay 1
        r XADD mystream IDMP p1 "req-2" * field "v2"
        set reply [r XINFO STREAM mystream]
        assert_equal 1 [dict get $reply pids-tracked]
        
        # Add entry with second producer
        r XADD mystream IDMP p2 "req-1" * field "v3"
        set reply [r XINFO STREAM mystream]
        assert_equal 2 [dict get $reply pids-tracked]
        
        # Add entry with third producer
        r XADD mystream IDMP producer3 "req-1" * field "v4"
        set reply [r XINFO STREAM mystream]
        assert_equal 3 [dict get $reply pids-tracked]
    }

    test {XINFO STREAM FULL returns pids-tracked field} {
        r DEL mystream
        
        # Add entries with multiple producers
        r XADD mystream IDMP prod-a "req-1" * field "v1"
        r XADD mystream IDMP prod-b "req-1" * field "v2"
        r XADD mystream IDMP prod-c "req-1" * field "v3"
        
        # Verify FULL mode also returns pids-tracked
        set reply [r XINFO STREAM mystream FULL]
        assert_equal 3 [dict get $reply pids-tracked]
    }

    test {XINFO STREAM iids-tracked counts across all producers} {
        r DEL mystream
        
        # Add entries with multiple producers
        r XADD mystream IDMP p1 "req-1" * field "v1"
        r XADD mystream IDMP p1 "req-2" * field "v2"
        r XADD mystream IDMP p2 "req-1" * field "v3"
        r XADD mystream IDMP p2 "req-2" * field "v4"
        r XADD mystream IDMP p2 "req-3" * field "v5"
        
        # iids-tracked should count all IIDs across all producers (2 + 3 = 5)
        set reply [r XINFO STREAM mystream]
        assert_equal 2 [dict get $reply pids-tracked]
        assert_equal 5 [dict get $reply iids-tracked]
        assert_equal 5 [dict get $reply iids-added]
        
        # Duplicates should not increment counters
        r XADD mystream IDMP p1 "req-1" * field "dup"
        r XADD mystream IDMP p2 "req-2" * field "dup"
        
        set reply [r XINFO STREAM mystream]
        assert_equal 2 [dict get $reply pids-tracked]
        assert_equal 5 [dict get $reply iids-tracked]
        assert_equal 5 [dict get $reply iids-added]
    }

    test {XINFO STREAM returns idmp-duration and idmp-maxsize fields} {
        r DEL mystream
        
        # Create stream with default IDMP config
        r XADD mystream IDMP p1 "req-1" * field "value1"
        
        # Verify default values
        set reply [r XINFO STREAM mystream]
        assert [dict exists $reply idmp-duration]
        assert [dict exists $reply idmp-maxsize]
        
        # Get default values from server config
        set default_duration [lindex [r CONFIG GET stream-idmp-duration] 1]
        set default_maxsize [lindex [r CONFIG GET stream-idmp-maxsize] 1]
        
        assert_equal $default_duration [dict get $reply idmp-duration]
        assert_equal $default_maxsize [dict get $reply idmp-maxsize]
        
        # Change IDMP config
        r XCFGSET mystream IDMP-DURATION 300 IDMP-MAXSIZE 50
        
        set reply [r XINFO STREAM mystream]
        assert_equal 300 [dict get $reply idmp-duration]
        assert_equal 50 [dict get $reply idmp-maxsize]
        
        # Also verify FULL mode returns the same fields
        set reply_full [r XINFO STREAM mystream FULL]
        assert_equal 300 [dict get $reply_full idmp-duration]
        assert_equal 50 [dict get $reply_full idmp-maxsize]
        
        # Change only DURATION
        r XCFGSET mystream IDMP-DURATION 600
        set reply [r XINFO STREAM mystream]
        assert_equal 600 [dict get $reply idmp-duration]
        assert_equal 50 [dict get $reply idmp-maxsize]
        
        # Change only MAXSIZE
        r XCFGSET mystream IDMP-MAXSIZE 100
        set reply [r XINFO STREAM mystream]
        assert_equal 600 [dict get $reply idmp-duration]
        assert_equal 100 [dict get $reply idmp-maxsize]
    }

    test {XCFGSET IDMP-MAXSIZE wraparound keeps last 8 entries} {
        r DEL mystream
        
        # Create stream and set MAXSIZE to 8
        r XADD mystream IDMP p1 "init" * field "init"
        r XCFGSET mystream IDMP-MAXSIZE 8 IDMP-DURATION 60
        
        # Add 100 unique entries and store their IDs in a list
        set id_list {}
        for {set i 1} {$i <= 100} {incr i} {
            lappend id_list [r XADD mystream IDMP p1 "req-$i" * field "v$i"]
        }
        
        # Verify the last 8 entries (93-100) still deduplicate
        for {set i 93} {$i <= 100} {incr i} {
            set idx [expr {$i - 1}]
            set original_id [lindex $id_list $idx]
            set dup_id [r XADD mystream IDMP p1 "req-$i" * field "dup"]
            assert_equal $original_id $dup_id
        }
        
        # Verify earlier entries (1-92) are evicted and create new entries
        for {set i 1} {$i <= 92} {incr i} {
            set idx [expr {$i - 1}]
            set original_id [lindex $id_list $idx]
            set new_id [r XADD mystream IDMP p1 "req-$i" * field "new"]
            assert {$new_id ne $original_id}
        }
        
        # Total entries: init + 100 original + 92 new = 193
        assert_equal 193 [r XLEN mystream]
    }

    test {XCFGSET clears all producer histories} {
        r DEL mystream
        
        # Add entries with multiple producers
        set id1 [r XADD mystream IDMP p1 "req-1" * field "v1"]
        set id2 [r XADD mystream IDMP p2 "req-1" * field "v2"]
        set id3 [r XADD mystream IDMP p3 "req-1" * field "v3"]
        
        set reply [r XINFO STREAM mystream]
        assert_equal 3 [dict get $reply pids-tracked]
        assert_equal 3 [dict get $reply iids-tracked]
        
        # CFGSET clears all histories
        r XCFGSET mystream IDMP-DURATION 60
        
        set reply [r XINFO STREAM mystream]
        # pids-tracked should be 0 after clearing
        assert_equal 0 [dict get $reply pids-tracked]
        assert_equal 0 [dict get $reply iids-tracked]
        # iids-added is lifetime counter, should persist
        assert_equal 3 [dict get $reply iids-added]
        
        # Can now add "duplicates" since history is cleared
        set new_id1 [r XADD mystream IDMP p1 "req-1" * field "new"]
        assert {$id1 ne $new_id1}
    }

    test {CONFIG SET stream-idmp-duration and stream-idmp-maxsize validation} {
        # Test maximum value rejection for duration (max: 86400)
        assert_error "*must be between*" {r CONFIG SET stream-idmp-duration 86401}
        assert_error "*must be between*" {r CONFIG SET stream-idmp-duration 100000}
        
        # Test maximum value rejection for maxsize (max: 10000)
        assert_error "*must be between*" {r CONFIG SET stream-idmp-maxsize 10001}
        assert_error "*must be between*" {r CONFIG SET stream-idmp-maxsize 50000}
        
        # Test minimum value rejection for duration (min: 1)
        assert_error "*must be between*" {r CONFIG SET stream-idmp-duration 0}
        assert_error "*must be between*" {r CONFIG SET stream-idmp-duration -1}
        assert_error "*must be between*" {r CONFIG SET stream-idmp-duration -100}
        
        # Test minimum value rejection for maxsize (min: 1)
        assert_error "*must be between*" {r CONFIG SET stream-idmp-maxsize 0}
        assert_error "*must be between*" {r CONFIG SET stream-idmp-maxsize -1}
        assert_error "*must be between*" {r CONFIG SET stream-idmp-maxsize -100}
        
        # Test exact boundary values work correctly
        assert_equal "OK" [r CONFIG SET stream-idmp-duration 86400]
        assert_equal "86400" [lindex [r CONFIG GET stream-idmp-duration] 1]
        
        assert_equal "OK" [r CONFIG SET stream-idmp-maxsize 10000]
        assert_equal "10000" [lindex [r CONFIG GET stream-idmp-maxsize] 1]
        
        # Test minimum boundary values work (min: 1)
        assert_equal "OK" [r CONFIG SET stream-idmp-duration 1]
        assert_equal "1" [lindex [r CONFIG GET stream-idmp-duration] 1]
        
        assert_equal "OK" [r CONFIG SET stream-idmp-maxsize 1]
        assert_equal "1" [lindex [r CONFIG GET stream-idmp-maxsize] 1]
        
        # Test valid intermediate values
        assert_equal "OK" [r CONFIG SET stream-idmp-duration 100]
        assert_equal "OK" [r CONFIG SET stream-idmp-maxsize 100]
        
        # Reset to defaults
        assert_equal "OK" [r CONFIG SET stream-idmp-duration 100]
        assert_equal "OK" [r CONFIG SET stream-idmp-maxsize 100]
    }

    test {XTRIM with MINID option} {
        r DEL mystream
        r XADD mystream 1-0 f v
        r XADD mystream 2-0 f v
        r XADD mystream 3-0 f v
        r XADD mystream 4-0 f v
        r XADD mystream 5-0 f v
        r XTRIM mystream MINID = 3-0
        assert_equal [r XRANGE mystream - +] {{3-0 {f v}} {4-0 {f v}} {5-0 {f v}}}
    }

    test {XTRIM with MINID option, big delta from master record} {
        r DEL mystream
        r XADD mystream 1-0 f v
        r XADD mystream 1641544570597-0 f v
        r XADD mystream 1641544570597-1 f v
        r XTRIM mystream MINID 1641544570597-0
        assert_equal [r XRANGE mystream - +] {{1641544570597-0 {f v}} {1641544570597-1 {f v}}}
    }

    proc insert_into_stream_key {key {count 10000}} {
        r multi
        for {set j 0} {$j < $count} {incr j} {
            # From time to time insert a field with a different set
            # of fields in order to stress the stream compression code.
            if {rand() < 0.9} {
                r XADD $key * item $j
            } else {
                r XADD $key * item $j otherfield foo
            }
        }
        r exec
    }

    test {XADD mass insertion and XLEN} {
        r DEL mystream
        insert_into_stream_key mystream

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

    test {XADD with LIMIT delete entries no more than limit} {
        r del yourstream
        for {set j 0} {$j < 3} {incr j} {
            r XADD yourstream * xitem v
        }
        r XADD yourstream MAXLEN ~ 0 limit 1 * xitem v
        assert {[r XLEN yourstream] == 4}
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

    test {XRANGE exclusive ranges} {
        set ids {0-1 0-18446744073709551615 1-0 42-0 42-42
                 18446744073709551615-18446744073709551614
                 18446744073709551615-18446744073709551615}
        set total [llength $ids]
        r multi
        r DEL vipstream
        foreach id $ids {
            r XADD vipstream $id foo bar
        }
        r exec
        assert {[llength [r xrange vipstream - +]] == $total}
        assert {[llength [r xrange vipstream ([lindex $ids 0] +]] == $total-1}
        assert {[llength [r xrange vipstream - ([lindex $ids $total-1]]] == $total-1}
        assert {[llength [r xrange vipstream (0-1 (1-0]] == 1}
        assert {[llength [r xrange vipstream (1-0 (42-42]] == 1}
        catch {r xrange vipstream (- +} e
        assert_match {ERR*} $e
        catch {r xrange vipstream - (+} e
        assert_match {ERR*} $e
        catch {r xrange vipstream (18446744073709551615-18446744073709551615 +} e
        assert_match {ERR*} $e
        catch {r xrange vipstream - (0-0} e
        assert_match {ERR*} $e
    }

    test {XREAD with non empty stream} {
        set res [r XREAD COUNT 1 STREAMS mystream 0-0]
        assert {[lrange [lindex $res 0 1 0 1] 0 1] eq {item 0}}
    }

    test {Non blocking XREAD with empty streams} {
        set res [r XREAD STREAMS s1{t} s2{t} 0-0 0-0]
        assert {$res eq {}}
    }

    test {XREAD with non empty second stream} {
        insert_into_stream_key mystream{t}
        set res [r XREAD COUNT 1 STREAMS nostream{t} mystream{t} 0-0 0-0]
        assert {[lindex $res 0 0] eq {mystream{t}}}
        assert {[lrange [lindex $res 0 1 0 1] 0 1] eq {item 0}}
    }

    test {Blocking XREAD waiting new data} {
        r XADD s2{t} * old abcd1234
        set rd [redis_deferring_client]
        $rd XREAD BLOCK 20000 STREAMS s1{t} s2{t} s3{t} $ $ $
        wait_for_blocked_client
        r XADD s2{t} * new abcd1234
        set res [$rd read]
        assert {[lindex $res 0 0] eq {s2{t}}}
        assert {[lindex $res 0 1 0 1] eq {new abcd1234}}
        $rd close
    }

    test {Blocking XREAD waiting old data} {
        set rd [redis_deferring_client]
        $rd XREAD BLOCK 20000 STREAMS s1{t} s2{t} s3{t} $ 0-0 $
        r XADD s2{t} * foo abcd1234
        set res [$rd read]
        assert {[lindex $res 0 0] eq {s2{t}}}
        assert {[lindex $res 0 1 0 1] eq {old abcd1234}}
        $rd close
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
        $rd close
    }

    test "Blocking XREAD for stream that ran dry (issue #5299)" {
        set rd [redis_deferring_client]

        # Add a entry then delete it, now stream's last_id is 666.
        r DEL mystream
        r XADD mystream 666 key value
        r XDEL mystream 666

        # Pass a ID smaller than stream's last_id, released on timeout.
        $rd XREAD BLOCK 10 STREAMS mystream 665
        assert_equal [$rd read] {}

        # Throw an error if the ID equal or smaller than the last_id.
        assert_error ERR*equal*smaller* {r XADD mystream 665 key value}
        assert_error ERR*equal*smaller* {r XADD mystream 666 key value}

        # Entered blocking state and then release because of the new entry.
        $rd XREAD BLOCK 0 STREAMS mystream 665
        wait_for_blocked_clients_count 1
        r XADD mystream 667 key value
        assert_equal [$rd read] {{mystream {{667-0 {key value}}}}}

        $rd close
    }

    test {XREAD last element from non-empty stream} {
        # should return last entry

        # add 3 entries to a stream
        r DEL lestream
        r XADD lestream 1-0 k1 v1
        r XADD lestream 2-0 k2 v2
        r XADD lestream 3-0 k3 v3

        # read the last entry
        set res [r XREAD STREAMS lestream +]

        # verify it's the last entry
        assert_equal $res {{lestream {{3-0 {k3 v3}}}}}

        # two more entries, with MAX_UINT64 for sequence number for the last one
        r XADD lestream 3-18446744073709551614 k4 v4
        r XADD lestream 3-18446744073709551615 k5 v5

        # read the new last entry
        set res [r XREAD STREAMS lestream +]

        # verify it's the last entry
        assert_equal $res {{lestream {{3-18446744073709551615 {k5 v5}}}}}
    }

    test {XREAD last element from empty stream} {
        # should return nil

        # make sure the stream is empty
        r DEL lestream

        # read last entry and verify nil is received
        assert_equal [r XREAD STREAMS lestream +] {}

        # add an element to the stream, than delete it
        r XADD lestream 1-0 k1 v1
        r XDEL lestream 1-0

        # verify nil is still received when reading last entry
        assert_equal [r XREAD STREAMS lestream +] {}

        # case when stream created empty

        # make sure the stream is not initialized
        r DEL lestream

        # create empty stream with XGROUP CREATE
        r XGROUP CREATE lestream legroup $ MKSTREAM

        # verify nil is received when reading last entry
        assert_equal [r XREAD STREAMS lestream +] {}
    }

    test {XREAD last element blocking from empty stream} {
        # should block until a new entry is available

        # make sure there is no stream
        r DEL lestream

        # read last entry from stream, blocking
        set rd [redis_deferring_client]
        $rd XREAD BLOCK 20000 STREAMS lestream +
        wait_for_blocked_client

        # add an entry to the stream
        r XADD lestream 1-0 k1 v1

        # read and verify result
        set res [$rd read]
        assert_equal $res {{lestream {{1-0 {k1 v1}}}}}
        $rd close
    }

    test {XREAD last element blocking from non-empty stream} {
        # should return last element immediately, w/o blocking

        # add 3 entries to a stream
        r DEL lestream
        r XADD lestream 1-0 k1 v1
        r XADD lestream 2-0 k2 v2
        r XADD lestream 3-0 k3 v3

        # read the last entry
        set res [r XREAD BLOCK 1000000 STREAMS lestream +]

        # verify it's the last entry
        assert_equal $res {{lestream {{3-0 {k3 v3}}}}}
    }

    test {XREAD last element from multiple streams} {
        # should return last element only from non-empty streams

        # add 3 entries to one stream
        r DEL "\{lestream\}1"
        r XADD "\{lestream\}1" 1-0 k1 v1
        r XADD "\{lestream\}1" 2-0 k2 v2
        r XADD "\{lestream\}1" 3-0 k3 v3

        # add 3 entries to another stream
        r DEL "\{lestream\}2"
        r XADD "\{lestream\}2" 1-0 k1 v4
        r XADD "\{lestream\}2" 2-0 k2 v5
        r XADD "\{lestream\}2" 3-0 k3 v6

        # read last element from 3 streams (2 with enetries, 1 non-existent)
        # verify the last element from the two existing streams were returned
        set res [r XREAD STREAMS "\{lestream\}1" "\{lestream\}2" "\{lestream\}3" + + +]
        assert_equal $res {{{{lestream}1} {{3-0 {k3 v3}}}} {{{lestream}2} {{3-0 {k3 v6}}}}}
    }

    test {XREAD last element with count > 1} {
        # Should return only the last element - count has no affect here

        # add 3 entries to a stream
        r DEL lestream
        r XADD lestream 1-0 k1 v1
        r XADD lestream 2-0 k2 v2
        r XADD lestream 3-0 k3 v3

        # read the last entry
        set res [r XREAD COUNT 3 STREAMS lestream +]

        # verify only last entry was read, even though COUNT > 1
        assert_equal $res {{lestream {{3-0 {k3 v3}}}}}
    }

    test "XREAD: read last element after XDEL (issue #13628)" {
        # Should return actual last element after XDEL of current last element

        # Add 2 entries to a stream and delete last one
        r DEL stream
        r XADD stream 1-0 f 1
        r XADD stream 2-0 f 2
        r XDEL stream 2-0

        # Read last entry
        set res [r XREAD STREAMS stream +]

        # Verify the last entry was read
        assert_equal $res {{stream {{1-0 {f 1}}}}}
    }

    test "XREAD: XADD + DEL should not awake client" {
        set rd [redis_deferring_client]
        r del s1
        $rd XREAD BLOCK 20000 STREAMS s1 $
        wait_for_blocked_clients_count 1
        r multi
        r XADD s1 * old abcd1234
        r DEL s1
        r exec
        r XADD s1 * new abcd1234
        set res [$rd read]
        assert {[lindex $res 0 0] eq {s1}}
        assert {[lindex $res 0 1 0 1] eq {new abcd1234}}
        $rd close
    }

    test "XREAD: XADD + DEL + LPUSH should not awake client" {
        set rd [redis_deferring_client]
        r del s1
        $rd XREAD BLOCK 20000 STREAMS s1 $
        wait_for_blocked_clients_count 1
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
        $rd close
    }

    test {XREAD with same stream name multiple times should work} {
        r XADD s2 * old abcd1234
        set rd [redis_deferring_client]
        $rd XREAD BLOCK 20000 STREAMS s2 s2 s2 $ $ $
        wait_for_blocked_clients_count 1
        r XADD s2 * new abcd1234
        set res [$rd read]
        assert {[lindex $res 0 0] eq {s2}}
        assert {[lindex $res 0 1 0 1] eq {new abcd1234}}
        $rd close
    }

    test {XREAD + multiple XADD inside transaction} {
        r XADD s2 * old abcd1234
        set rd [redis_deferring_client]
        $rd XREAD BLOCK 20000 STREAMS s2 s2 s2 $ $ $
        wait_for_blocked_clients_count 1
        r MULTI
        r XADD s2 * field one
        r XADD s2 * field two
        r XADD s2 * field three
        r EXEC
        set res [$rd read]
        assert {[lindex $res 0 0] eq {s2}}
        assert {[lindex $res 0 1 0 1] eq {field one}}
        assert {[lindex $res 0 1 1 1] eq {field two}}
        $rd close
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

    test {XDEL multiply id test} {
        r del somestream
        r xadd somestream 1-1 a 1
        r xadd somestream 1-2 b 2
        r xadd somestream 1-3 c 3
        r xadd somestream 1-4 d 4
        r xadd somestream 1-5 e 5
        assert {[r xlen somestream] == 5}
        assert {[r xdel somestream 1-1 1-4 1-5 2-1] == 3}
        assert {[r xlen somestream] == 2}
        set result [r xrange somestream - +]
        assert {[dict get [lindex $result 0 1] b] eq {2}}
        assert {[dict get [lindex $result 1 1] c] eq {3}}
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
        set items [r XRANGE mystream{t} - +]
        set low_id [lindex $items 0 0]
        set high_id [lindex $items end 0]
        for {set j 0} {$j < 100} {incr j} {
            set start [streamRandomID $low_id $high_id]
            set end [streamRandomID $low_id $high_id]
            set range [r xrange mystream{t} $start $end]
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
        wait_for_blocked_clients_count 1
        r XADD x 1-1 f v
        r XADD x 1-18446744073709551615 f v
        r XADD x 2-1 f v
        set res [$rd read]
        assert {[lindex $res 0 1 0] == {2-1 {f v}}}
        $rd close
    }

    test {XADD streamID edge} {
        r del x
        r XADD x 2577343934890-18446744073709551615 f v ;# we need the timestamp to be in the future
        r XADD x * f2 v2
        assert_equal [r XRANGE x - +] {{2577343934890-18446744073709551615 {f v}} {2577343934891-0 {f2 v2}}}
    }

    test {XTRIM with MAXLEN option basic test} {
        r DEL mystream
        for {set j 0} {$j < 1000} {incr j} {
            if {rand() < 0.9} {
                r XADD mystream * xitem $j
            } else {
                r XADD mystream * yitem $j
            }
        }
        r XTRIM mystream MAXLEN 666
        assert {[r XLEN mystream] == 666}
        r XTRIM mystream MAXLEN = 555
        assert {[r XLEN mystream] == 555}
        r XTRIM mystream MAXLEN ~ 444
        assert {[r XLEN mystream] == 500}
        r XTRIM mystream MAXLEN ~ 400
        assert {[r XLEN mystream] == 400}
    }

    test {XADD with LIMIT consecutive calls} {
        r del mystream
        r config set stream-node-max-entries 10
        for {set j 0} {$j < 100} {incr j} {
            r XADD mystream * xitem v
        }
        r XADD mystream MAXLEN ~ 55 LIMIT 30 * xitem v
        assert {[r xlen mystream] == 71}
        r XADD mystream MAXLEN ~ 55 LIMIT 30 * xitem v
        assert {[r xlen mystream] == 62}
        r config set stream-node-max-entries 100
    }

    test {XTRIM with ~ is limited} {
        r del mystream
        r config set stream-node-max-entries 1
        for {set j 0} {$j < 102} {incr j} {
            r XADD mystream * xitem v
        }
        r XTRIM mystream MAXLEN ~ 1
        assert {[r xlen mystream] == 2}
        r config set stream-node-max-entries 100
    }

    test {XTRIM without ~ is not limited} {
        r del mystream
        r config set stream-node-max-entries 1
        for {set j 0} {$j < 102} {incr j} {
            r XADD mystream * xitem v
        }
        r XTRIM mystream MAXLEN 1
        assert {[r xlen mystream] == 1}
        r config set stream-node-max-entries 100
    }

    test {XTRIM without ~ and with LIMIT} {
        r del mystream
        r config set stream-node-max-entries 1
        for {set j 0} {$j < 102} {incr j} {
            r XADD mystream * xitem v
        }
        assert_error ERR* {r XTRIM mystream MAXLEN 1 LIMIT 30}
    }

    test {XTRIM with LIMIT delete entries no more than limit} {
        r del mystream
        r config set stream-node-max-entries 2
        for {set j 0} {$j < 3} {incr j} {
            r XADD mystream * xitem v
        }
        assert {[r XTRIM mystream MAXLEN ~ 0 LIMIT 1] == 0}
        assert {[r XTRIM mystream MAXLEN ~ 0 LIMIT 2] == 2}
    }

    test {XTRIM with approx and ACKED deletes entries correctly} {
        # This test verifies that when using approx trim (~) with ACKED strategy,
        # if the first node cannot be removed (has unacked messages), we should
        # continue to check subsequent nodes that might be eligible for removal.
        r DEL mystream
        set origin_max_entries [config_get_set stream-node-max-entries 2]

        # Create 5 entries in 3 nodes (2 entries per node)
        r XADD mystream 1-0 f v
        r XADD mystream 2-0 f v
        r XADD mystream 3-0 f v
        r XADD mystream 4-0 f v
        r XADD mystream 5-0 f v

        # Create a consumer group and read all messages
        r XGROUP CREATE mystream mygroup 0
        r XREADGROUP GROUP mygroup consumer1 STREAMS mystream >

        # Acknowledge messages: 1-0, 2-0 (first node), and 4-0 (second node)
        r XACK mystream mygroup 1-0 2-0 4-0

        # XTRIM MINID ~ 6-0 ACKED should remove:
        # Total 3 entries removed (1-0, 2-0, 4-0), 2 unacked entries remain (3-0, 5-0)
        assert_equal 3 [r XTRIM mystream MINID ~ 6-0 ACKED]
        assert_equal 2 [r XLEN mystream]
        assert_equal {{3-0 {f v}} {5-0 {f v}}} [r XRANGE mystream - +]

        r config set stream-node-max-entries $origin_max_entries
    }

    test {XTRIM with approx and DELREF deletes entries correctly} {
        # Similar test but with DELREF strategy
        r DEL mystream
        set origin_max_entries [config_get_set stream-node-max-entries 2]

        # Create 4 entries in 2 nodes
        r XADD mystream 1-0 f v
        r XADD mystream 2-0 f v
        r XADD mystream 3-0 f v
        r XADD mystream 4-0 f v

        # Create a consumer group and read all messages
        r XGROUP CREATE mystream mygroup 0
        r XREADGROUP GROUP mygroup consumer1 STREAMS mystream >

        # With XTRIM MINID ~ 5-0 DELREF, all eligible nodes should be trimmed
        # and PEL entries should be cleaned up
        assert_equal 4 [r XTRIM mystream MINID ~ 5-0 DELREF]
        assert_equal 0 [r XLEN mystream]
        # PEL should be empty after DELREF
        assert_equal {0 {} {} {}} [r XPENDING mystream mygroup]

        r config set stream-node-max-entries $origin_max_entries
    }
}

start_server {tags {"stream needs:debug"} overrides {appendonly yes}} {
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

start_server {tags {"stream needs:debug"} overrides {appendonly yes}} {
    test {XADD with MINID > lastid can propagate correctly} {
        for {set j 0} {$j < 100} {incr j} {
            set id [expr {$j+1}]
            r XADD mystream $id xitem v
        }
        r XADD mystream MINID 1 * xitem v
        incr j
        assert {[r xlen mystream] == $j}
        r debug loadaof
        r XADD mystream * xitem v
        incr j
        assert {[r xlen mystream] == $j}
    }
}

start_server {tags {"stream needs:debug"} overrides {appendonly yes stream-node-max-entries 100}} {
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

start_server {tags {"stream needs:debug"} overrides {appendonly yes stream-node-max-entries 10}} {
    test {XADD with ~ MAXLEN and LIMIT can propagate correctly} {
        for {set j 0} {$j < 100} {incr j} {
            r XADD mystream * xitem v
        }
        r XADD mystream MAXLEN ~ 55 LIMIT 30 * xitem v
        assert {[r xlen mystream] == 71}
        r config set stream-node-max-entries 1
        r debug loadaof
        r XADD mystream * xitem v
        assert {[r xlen mystream] == 72}
    }
}

start_server {tags {"stream needs:debug"} overrides {appendonly yes stream-node-max-entries 100}} {
    test {XADD with ~ MINID can propagate correctly} {
        for {set j 0} {$j < 100} {incr j} {
            set id [expr {$j+1}]
            r XADD mystream $id xitem v
        }
        r XADD mystream MINID ~ $j * xitem v
        incr j
        assert {[r xlen mystream] == $j}
        r config set stream-node-max-entries 1
        r debug loadaof
        r XADD mystream * xitem v
        incr j
        assert {[r xlen mystream] == $j}
    }
}

start_server {tags {"stream needs:debug"} overrides {appendonly yes stream-node-max-entries 10}} {
    test {XADD with ~ MINID and LIMIT can propagate correctly} {
        for {set j 0} {$j < 100} {incr j} {
            set id [expr {$j+1}]
            r XADD mystream $id xitem v
        }
        r XADD mystream MINID ~ 55 LIMIT 30 * xitem v
        assert {[r xlen mystream] == 71}
        r config set stream-node-max-entries 1
        r debug loadaof
        r XADD mystream * xitem v
        assert {[r xlen mystream] == 72}
    }
}

start_server {tags {"stream needs:debug"} overrides {appendonly yes stream-node-max-entries 10}} {
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

start_server {tags {"stream"}} {
    test {XADD can CREATE an empty stream} {
        r XADD mystream MAXLEN 0 * a b
        assert {[dict get [r xinfo stream mystream] length] == 0}
    }

    test {XSETID can set a specific ID} {
        r XSETID mystream "200-0"
        set reply [r XINFO stream mystream]
        assert_equal [dict get $reply last-generated-id] "200-0"
        assert_equal [dict get $reply entries-added] 1
    }

    test {XSETID cannot SETID with smaller ID} {
        r XADD mystream * a b
        catch {r XSETID mystream "1-1"} err
        r XADD mystream MAXLEN 0 * a b
        set err
    } {ERR *smaller*}

    test {XSETID cannot SETID on non-existent key} {
        catch {r XSETID stream 1-1} err
        set _ $err
    } {ERR no such key}

    test {XSETID cannot run with an offset but without a maximal tombstone} {
        catch {r XSETID stream 1-1 0} err
        set _ $err
    } {ERR syntax error}

    test {XSETID cannot run with a maximal tombstone but without an offset} {
        catch {r XSETID stream 1-1 0-0} err
        set _ $err
    } {ERR syntax error}

    test {XSETID errors on negstive offset} {
        catch {r XSETID stream 1-1 ENTRIESADDED -1 MAXDELETEDID 0-0} err
        set _ $err
    } {ERR *must be positive}

    test {XSETID cannot set the maximal tombstone with larger ID} {
        r DEL x
        r XADD x 1-0 a b
        
        catch {r XSETID x "1-0" ENTRIESADDED 1 MAXDELETEDID "2-0" } err
        r XADD mystream MAXLEN 0 * a b
        set err
    } {ERR *smaller*}

    test {XSETID cannot set the offset to less than the length} {
        r DEL x
        r XADD x 1-0 a b
        
        catch {r XSETID x "1-0" ENTRIESADDED 0 MAXDELETEDID "0-0" } err
        r XADD mystream MAXLEN 0 * a b
        set err
    } {ERR *smaller*}

    test {XSETID cannot set smaller ID than current MAXDELETEDID} {
        r DEL x
        r XADD x 1-1 a 1
        r XADD x 1-2 b 2
        r XADD x 1-3 c 3
        r XDEL x 1-2
        r XDEL x 1-3
        set reply [r XINFO stream x]
        assert_equal [dict get $reply max-deleted-entry-id] "1-3"
        catch {r XSETID x "1-2" } err
        set err
    } {ERR *smaller*}
}

start_server {tags {"stream"}} {
    test {XADD advances the entries-added counter and sets the recorded-first-entry-id} {
        r DEL x
        r XADD x 1-0 data a

        set reply [r XINFO STREAM x FULL]
        assert_equal [dict get $reply entries-added] 1
        assert_equal [dict get $reply recorded-first-entry-id] "1-0"

        r XADD x 2-0 data a
        set reply [r XINFO STREAM x FULL]
        assert_equal [dict get $reply entries-added] 2
        assert_equal [dict get $reply recorded-first-entry-id] "1-0"
    }

    test {XDEL/TRIM are reflected by recorded first entry} {
        r DEL x
        r XADD x 1-0 data a
        r XADD x 2-0 data a
        r XADD x 3-0 data a
        r XADD x 4-0 data a
        r XADD x 5-0 data a

        set reply [r XINFO STREAM x FULL]
        assert_equal [dict get $reply entries-added] 5
        assert_equal [dict get $reply recorded-first-entry-id] "1-0"

        r XDEL x 2-0
        set reply [r XINFO STREAM x FULL]
        assert_equal [dict get $reply recorded-first-entry-id] "1-0"

        r XDEL x 1-0
        set reply [r XINFO STREAM x FULL]
        assert_equal [dict get $reply recorded-first-entry-id] "3-0"

        r XTRIM x MAXLEN = 2
        set reply [r XINFO STREAM x FULL]
        assert_equal [dict get $reply recorded-first-entry-id] "4-0"
    }

    test {Maximum XDEL ID behaves correctly} {
        r DEL x
        r XADD x 1-0 data a
        r XADD x 2-0 data b
        r XADD x 3-0 data c

        set reply [r XINFO STREAM x FULL]
        assert_equal [dict get $reply max-deleted-entry-id] "0-0"

        r XDEL x 2-0
        set reply [r XINFO STREAM x FULL]
        assert_equal [dict get $reply max-deleted-entry-id] "2-0"

        r XDEL x 1-0
        set reply [r XINFO STREAM x FULL]
        assert_equal [dict get $reply max-deleted-entry-id] "2-0"
    }

    test {XADD with artial ID with maximal seq} {
        r DEL x
        r XADD x 1-18446744073709551615 f1 v1
        assert_error {*The ID specified in XADD is equal or smaller*} {r XADD x 1-* f2 v2}
    }
}

start_server {tags {"stream needs:debug"} overrides {appendonly yes aof-use-rdb-preamble no}} {
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
        assert_equal [dict get [r xinfo stream mystream] last-generated-id] "2-2"
    }
}

start_server {tags {"stream"}} {
    test {XGROUP HELP should not have unexpected options} {
        catch {r XGROUP help xxx} e
        assert_match "*wrong number of arguments for 'xgroup|help' command" $e
    }

    test {XINFO HELP should not have unexpected options} {
        catch {r XINFO help xxx} e
        assert_match "*wrong number of arguments for 'xinfo|help' command" $e
    }
}

start_server {tags {"stream"}} {
    test "XDELEX wrong number of args" {
        assert_error {*wrong number of arguments for 'xdelex' command} {r XDELEX s DELREF}
    }

    test "XDELEX should return empty array when key doesn't exist" {
        r DEL nonexist
        assert_equal {-1 -1} [r XDELEX nonexist IDS 2 1-1 2-2]
    }

    test "XDELEX IDS parameter validation" {
        r DEL s
        r XADD s 1-0 f v
        r XGROUP CREATE s g 0

        # Test invalid numids
        assert_error {*Number of IDs must be a positive integer*} {r XDELEX s IDS abc 1-1}
        assert_error {*Number of IDs must be a positive integer*} {r XDELEX s IDS 0 1-1}
        assert_error {*Number of IDs must be a positive integer*} {r XDELEX s IDS -5 1-1}

        # Test whether numids is equal to the number of IDs provided
        assert_error {*The `numids` parameter must match the number of arguments*} {r XDELEX s IDS 3 1-1 2-2}
        assert_error {*syntax error*} {r XDELEX s IDS 1 1-1 2-2}

        # Delete non-existent ids
        assert_equal {-1 -1} [r XDELEX s IDS 2 1-1 2-2]
    }

    test "XDELEX KEEPREF/DELREF/ACKED parameter validation" {
        # Test mutually exclusive options
        assert_error {*syntax error*} {r XDELEX s KEEPREF DELREF IDS 1 1-1}
        assert_error {*syntax error*} {r XDELEX s KEEPREF ACKED IDS 1 1-1}
        assert_error {*syntax error*} {r XDELEX s ACKED DELREF IDS 1 1-1}
    }

    test "XDELEX with DELREF option acknowledges will remove entry from all PELs" {
        r DEL mystream
        r XADD mystream 1-0 f v
        r XADD mystream 2-0 f v

        # Create two consumer groups
        r XGROUP CREATE mystream group1 0
        r XGROUP CREATE mystream group2 0
        r XREADGROUP GROUP group1 consumer1 STREAMS mystream >
        r XREADGROUP GROUP group2 consumer2 STREAMS mystream >

        # Verify the message was removed from both groups' PELs when with DELREF
        assert_equal {1 1} [r XDELEX mystream DELREF IDS 2 1-0 2-0]
        assert_equal 0 [r XLEN mystream] 
        assert_equal {0 {} {} {}} [r XPENDING mystream group1]
        assert_equal {0 {} {} {}} [r XPENDING mystream group2] 
    }

    test "XDELEX with ACKED option only deletes messages acknowledged by all groups" {
        r DEL mystream
        r XADD mystream 1-0 f v
        r XADD mystream 2-0 f v

        # Create two consumer groups
        r XGROUP CREATE mystream group1 0
        r XGROUP CREATE mystream group2 0
        r XGROUP CREATE mystream group3 0
        r XREADGROUP GROUP group1 consumer1 STREAMS mystream >
        r XREADGROUP GROUP group2 consumer2 STREAMS mystream >

        # The message is referenced by three consumer groups:
        # - group1 and group2 have read the messages
        # - group3 hasn't read the messages yet (not delivered)
        # Even after group1 acknowledges the messages, they still can't be deleted
        r XACK mystream group1 1-0 2-0
        assert_equal {2 2} [r XDELEX mystream ACKED IDS 2 1-0 2-0]
        assert_equal 2 [r XLEN mystream]

        # Even after both group1 and group2 acknowledge the messages, these entries
        # still can't be deleted because group3 hasn't even read them yet.
        r XACK mystream group2 1-0 2-0
        assert_equal {2 2} [r XDELEX mystream ACKED IDS 2 1-0 2-0]
        assert_equal 2 [r XLEN mystream]

        # Now group3 reads the messages, but hasn't acknowledged them yet.
        # these entries still can't be deleted because group3 hasn't acknowledged them.
        r XREADGROUP GROUP group3 consumer3 STREAMS mystream >
        assert_equal {2 2} [r XDELEX mystream ACKED IDS 2 1-0 2-0]
        assert_equal 2 [r XLEN mystream]

        # Now group3 acknowledges the messages. These entries can now be deleted.
        r XACK mystream group3 1-0 2-0
        r XDELEX mystream ACKED IDS 2 1-0 2-0
        assert_equal 0 [r XLEN mystream]
    }

    test "XDELEX with ACKED option won't delete messages when new consumer groups are created" {
        r DEL mystream
        r XADD mystream 1-0 f v
        r XADD mystream 2-0 f v
        r XADD mystream 3-0 f v

        r XGROUP CREATE mystream group1 0
        r XREADGROUP GROUP group1 consumer1 STREAMS mystream >

        # When the group1 ack message, the message can be deleted with ACK option.
        assert_equal {3} [r XACK mystream group1 1-0 2-0 3-0]
        assert_equal {1} [r XDELEX mystream ACKED IDS 1 1-0]
        
        # Create a new consumer group that hasn't read the messages yet.
        # Even if group1 ack the message, we still can't delete the message.
        r XGROUP CREATE mystream group2 0
        assert_equal {2} [r XDELEX mystream ACKED IDS 1 2-0]

        # Now group2 reads and acknowledges the messages,
        # so we can be successfully deleted with the ACKED option.
        r XREADGROUP GROUP group2 consumer1 STREAMS mystream >
        assert_equal {2} [r XACK mystream group2 2-0 3-0]
        assert_equal {1 1} [r XDELEX mystream ACKED IDS 2 2-0 3-0]
    }

    test "XDELEX with KEEPREF" {
        r DEL mystream
        r XADD mystream 1-0 f v
        r XADD mystream 2-0 f v

        # Create two consumer groups
        r XGROUP CREATE mystream group1 0
        r XGROUP CREATE mystream group2 0
        r XREADGROUP GROUP group1 consumer1 STREAMS mystream >
        r XREADGROUP GROUP group2 consumer2 STREAMS mystream >

        # Test XDELEX with KEEPREF
        # XDELEX only deletes the message from the stream
        # but does not clean up references in consumer groups' PELs
        assert_equal {1 1} [r XDELEX mystream KEEPREF IDS 2 1-0 2-0]
        assert_equal 0 [r XLEN mystream]
        assert_equal {2 1-0 2-0 {{consumer1 2}}} [r XPENDING mystream group1]
        assert_equal {2 1-0 2-0 {{consumer2 2}}} [r XPENDING mystream group2]
    }
}
