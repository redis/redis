start_server {
    tags {"stream"}
} {
    test {XGROUP CREATE: creation and duplicate group name detection} {
        r DEL mystream
        r XADD mystream * foo bar
        r XGROUP CREATE mystream mygroup $
        catch {r XGROUP CREATE mystream mygroup $} err
        set err
    } {BUSYGROUP*}

    test {XGROUP CREATE: with ENTRIESREAD parameter} {
        r DEL mystream
        r XADD mystream 1-1 a 1
        r XADD mystream 1-2 b 2
        r XADD mystream 1-3 c 3
        r XADD mystream 1-4 d 4
        assert_error "*value for ENTRIESREAD must be positive or -1*" {r XGROUP CREATE mystream mygroup $ ENTRIESREAD -3}

        r XGROUP CREATE mystream mygroup1 $ ENTRIESREAD 0
        r XGROUP CREATE mystream mygroup2 $ ENTRIESREAD 3

        set reply [r xinfo groups mystream]
        foreach group_info $reply {
            set group_name [dict get $group_info name]
            set entries_read [dict get $group_info entries-read]
            if {$group_name == "mygroup1"} {
                assert_equal $entries_read 0
            } else {
                assert_equal $entries_read 3
            }
        }
    }

    test {XGROUP CREATE: automatic stream creation fails without MKSTREAM} {
        r DEL mystream
        catch {r XGROUP CREATE mystream mygroup $} err
        set err
    } {ERR*}

    test {XGROUP CREATE: automatic stream creation works with MKSTREAM} {
        r DEL mystream
        r XGROUP CREATE mystream mygroup $ MKSTREAM
    } {OK}

    test {XREADGROUP basic argument count validation} {
        # Too few arguments
        assert_error "*wrong number of arguments*" {r XREADGROUP}
        assert_error "*wrong number of arguments*" {r XREADGROUP GROUP}
        assert_error "*wrong number of arguments*" {r XREADGROUP GROUP mygroup}
        assert_error "*wrong number of arguments*" {r XREADGROUP GROUP mygroup consumer}
        assert_error "*wrong number of arguments*" {r XREADGROUP GROUP mygroup consumer STREAMS}
    }

    test {XREADGROUP GROUP keyword validation} {
        r DEL mystream
        r XADD mystream * field value
        r XGROUP CREATE mystream mygroup $
        
        # Missing GROUP keyword entirely - wrong syntax
        assert_error "*wrong number of arguments*" {r XREADGROUP mygroup consumer STREAMS mystream >}
        
        # Wrong keyword instead of GROUP
        assert_error "*syntax error*" {r XREADGROUP GROUPS mygroup consumer STREAMS mystream >}
    }

    test {XREADGROUP empty group name handling} {
        r DEL mystream
        r XADD mystream * field value
        r XGROUP CREATE mystream mygroup $
        
        # Empty group name should give NOGROUP error
        assert_error "*NOGROUP*" {r XREADGROUP GROUP "" consumer STREAMS mystream >}
    }

    test {XREADGROUP STREAMS keyword validation} {
        r DEL mystream
        r XADD mystream * field value
        r XGROUP CREATE mystream mygroup $
        
        # Missing STREAMS keyword
        assert_error "*wrong number of arguments*" {r XREADGROUP GROUP mygroup consumer mystream >}
        
        # Wrong keyword
        assert_error "*syntax error*" {r XREADGROUP GROUP mygroup consumer STREAM mystream >}
    }

    test {XREADGROUP stream and ID pairing} {
        r DEL mystream
        r XADD mystream * field value
        r XGROUP CREATE mystream mygroup $
        
        # Missing stream ID
        assert_error "*wrong number of arguments*" {r XREADGROUP GROUP mygroup consumer STREAMS mystream}
        
        # Unbalanced streams and IDs
        r DEL stream2
        r XADD stream2 * field value
        r XGROUP CREATE stream2 mygroup $
        
        assert_error "*Unbalanced*" {r XREADGROUP GROUP mygroup consumer STREAMS mystream > stream2}
        assert_error "*Unbalanced*" {r XREADGROUP GROUP mygroup consumer STREAMS mystream stream2 >}
        
        r DEL stream2
    }

    test {XREADGROUP COUNT parameter validation} {
        r DEL mystream
        r XADD mystream * field value
        r XGROUP CREATE mystream mygroup $
        
        # Non-numeric count
        assert_error "*not an integer*" {r XREADGROUP GROUP mygroup consumer COUNT abc STREAMS mystream >}
        assert_error "*not an integer*" {r XREADGROUP GROUP mygroup consumer COUNT 1.5 STREAMS mystream >}
    }

    test {XREADGROUP BLOCK parameter validation} {
        r DEL mystream
        r XADD mystream * field value
        r XGROUP CREATE mystream mygroup $
        
        # Non-numeric block timeout
        assert_error "*not an integer*" {r XREADGROUP GROUP mygroup consumer BLOCK abc STREAMS mystream >}
        assert_error "*not an integer*" {r XREADGROUP GROUP mygroup consumer BLOCK 1.5 STREAMS mystream >}
        
        # Missing BLOCK value
        assert_error "*ERR timeout is not an integer or out of range*" {r XREADGROUP GROUP mygroup consumer BLOCK STREAMS mystream >}
        
        # Negative timeout (typically not allowed)
        assert_error "*ERR timeout is negative*" {r XREADGROUP GROUP mygroup consumer BLOCK -1 STREAMS mystream >}
    }

    test {XREADGROUP stream ID format validation} {
        r DEL mystream
        r XADD mystream * field value
        r XGROUP CREATE mystream mygroup $
        
        # Invalid ID formats should error
        assert_error "*Invalid stream ID*" {r XREADGROUP GROUP mygroup consumer STREAMS mystream invalid-id}
        assert_error "*Invalid stream ID*" {r XREADGROUP GROUP mygroup consumer STREAMS mystream 123-}
        assert_error "*Invalid stream ID*" {r XREADGROUP GROUP mygroup consumer STREAMS mystream -123}
        assert_error "*Invalid stream ID*" {r XREADGROUP GROUP mygroup consumer STREAMS mystream abc-def}
        assert_error "*Invalid stream ID*" {r XREADGROUP GROUP mygroup consumer STREAMS mystream --}
        assert_error "*Invalid stream ID*" {r XREADGROUP GROUP mygroup consumer STREAMS mystream 123-abc}
    }

    test {XREADGROUP nonexistent group} {
        r DEL mystream
        r XADD mystream * field value
        r XGROUP CREATE mystream mygroup $
        
        assert_error "*NOGROUP*" {r XREADGROUP GROUP nonexistent consumer STREAMS mystream >}
    }

    test {XREADGROUP nonexistent stream with existing group} {
        r DEL mystream
        r XADD mystream * field value
        r XGROUP CREATE mystream mygroup $
        
        # Group doesn't exist on the nonexistent stream
        assert_error "*NOGROUP*" {r XREADGROUP GROUP mygroup consumer STREAMS nonexistent >}
    }

    test {XREADGROUP wrong key type} {
        r SET wrongtype "not a stream"
        assert_error "*WRONGTYPE*" {r XREADGROUP GROUP mygroup consumer STREAMS wrongtype >}
        r DEL wrongtype
    }

    test {XREADGROUP boundary value validation} {
        r DEL mystream
        r XADD mystream * field value
        r XGROUP CREATE mystream mygroup $
        
        # Test COUNT boundaries - values that are too large
        assert_error "*value is not an integer or out of range*" {r XREADGROUP GROUP mygroup consumer COUNT 18446744073709551616 STREAMS mystream >}
        
        # Test BLOCK timeout boundaries - values that are too large  
        assert_error "*timeout is not an integer or out of range*" {r XREADGROUP GROUP mygroup consumer BLOCK 18446744073709551616 STREAMS mystream >}
    }

    test {XREADGROUP malformed parameter syntax} {
        r DEL mystream
        r XADD mystream * field value
        r XGROUP CREATE mystream mygroup $
        
        # Unknown parameters
        assert_error "*syntax error*" {r XREADGROUP GROUP mygroup consumer INVALID param STREAMS mystream >}
        assert_error "*syntax error*" {r XREADGROUP GROUP mygroup consumer TIMEOUT 1000 STREAMS mystream >}
    }

    test {XREADGROUP will return only new elements} {
        r XADD mystream * a 1
        r XADD mystream * b 2

        # Verify XPENDING returns empty results when no messages are in the PEL.
        assert_equal {0 {} {} {}} [r XPENDING mystream mygroup]
        assert_equal {} [r XPENDING mystream mygroup - + 10] 

        # XREADGROUP should return only the new elements "a 1" "b 1"
        # and not the element "foo bar" which was pre existing in the
        # stream (see previous test)
        set reply [
            r XREADGROUP GROUP mygroup consumer-1 STREAMS mystream ">"
        ]
        assert {[llength [lindex $reply 0 1]] == 2}
        lindex $reply 0 1 0 1
    } {a 1}

    test {XREADGROUP can read the history of the elements we own} {
        # Add a few more elements
        r XADD mystream * c 3
        r XADD mystream * d 4
        # Read a few elements using a different consumer name
        set reply [
            r XREADGROUP GROUP mygroup consumer-2 STREAMS mystream ">"
        ]
        assert {[llength [lindex $reply 0 1]] == 2}
        assert {[lindex $reply 0 1 0 1] eq {c 3}}

        set r1 [r XREADGROUP GROUP mygroup consumer-1 COUNT 10 STREAMS mystream 0]
        set r2 [r XREADGROUP GROUP mygroup consumer-2 COUNT 10 STREAMS mystream 0]
        assert {[lindex $r1 0 1 0 1] eq {a 1}}
        assert {[lindex $r2 0 1 0 1] eq {c 3}}
    }

    test {XPENDING is able to return pending items} {
        set pending [r XPENDING mystream mygroup - + 10]
        assert {[llength $pending] == 4}
        for {set j 0} {$j < 4} {incr j} {
            set item [lindex $pending $j]
            if {$j < 2} {
                set owner consumer-1
            } else {
                set owner consumer-2
            }
            assert {[lindex $item 1] eq $owner}
            assert {[lindex $item 1] eq $owner}
        }
    }

    test {XPENDING can return single consumer items} {
        set pending [r XPENDING mystream mygroup - + 10 consumer-1]
        assert {[llength $pending] == 2}
    }

    test {XPENDING only group} {
        set pending [r XPENDING mystream mygroup]
        assert {[llength $pending] == 4}
    }

    test {XPENDING with IDLE} {
        after 20
        set pending [r XPENDING mystream mygroup IDLE 99999999 - + 10 consumer-1]
        assert {[llength $pending] == 0}
        set pending [r XPENDING mystream mygroup IDLE 1 - + 10 consumer-1]
        assert {[llength $pending] == 2}
        set pending [r XPENDING mystream mygroup IDLE 99999999 - + 10]
        assert {[llength $pending] == 0}
        set pending [r XPENDING mystream mygroup IDLE 1 - + 10]
        assert {[llength $pending] == 4}
    }

    test {XPENDING with exclusive range intervals works as expected} {
        set pending [r XPENDING mystream mygroup - + 10]
        assert {[llength $pending] == 4}
        set startid [lindex [lindex $pending 0] 0]
        set endid [lindex [lindex $pending 3] 0]
        set expending [r XPENDING mystream mygroup ($startid ($endid 10]
        assert {[llength $expending] == 2}
        for {set j 0} {$j < 2} {incr j} {
            set itemid [lindex [lindex $expending $j] 0]
            assert {$itemid ne $startid}
            assert {$itemid ne $endid}
        }
    }

    test {XACK is able to remove items from the consumer/group PEL} {
        set pending [r XPENDING mystream mygroup - + 10 consumer-1]
        set id1 [lindex $pending 0 0]
        set id2 [lindex $pending 1 0]
        assert {[r XACK mystream mygroup $id1] eq 1}
        set pending [r XPENDING mystream mygroup - + 10 consumer-1]
        assert {[llength $pending] == 1}
        set id [lindex $pending 0 0]
        assert {$id eq $id2}
        set global_pel [r XPENDING mystream mygroup - + 10]
        assert {[llength $global_pel] == 3}
    }

    test {XACK can't remove the same item multiple times} {
        assert {[r XACK mystream mygroup $id1] eq 0}
    }

    test {XACK is able to accept multiple arguments} {
        # One of the IDs was already removed, so it should ack
        # just ID2.
        assert {[r XACK mystream mygroup $id1 $id2] eq 1}
    }

    test {XACK should fail if got at least one invalid ID} {
        r del mystream
        r xgroup create s g $ MKSTREAM
        r xadd s * f1 v1
        set c [llength [lindex [r xreadgroup group g c streams s >] 0 1]]
        assert {$c == 1}
        set pending [r xpending s g - + 10 c]
        set id1 [lindex $pending 0 0]
        assert_error "*Invalid stream ID specified*" {r xack s g $id1 invalid-id}
        assert {[r xack s g $id1] eq 1}
    }

    test {PEL NACK reassignment after XGROUP SETID event} {
        r del events
        r xadd events * f1 v1
        r xadd events * f1 v1
        r xadd events * f1 v1
        r xadd events * f1 v1
        r xgroup create events g1 $
        r xadd events * f1 v1
        set c [llength [lindex [r xreadgroup group g1 c1 streams events >] 0 1]]
        assert {$c == 1}
        r xgroup setid events g1 -
        set c [llength [lindex [r xreadgroup group g1 c2 streams events >] 0 1]]
        assert {$c == 5}
    }

    test {XREADGROUP will not report data on empty history. Bug #5577} {
        r del events
        r xadd events * a 1
        r xadd events * b 2
        r xadd events * c 3
        r xgroup create events mygroup 0

        # Current local PEL should be empty
        set res [r xpending events mygroup - + 10]
        assert {[llength $res] == 0}

        # So XREADGROUP should read an empty history as well
        set res [r xreadgroup group mygroup myconsumer count 3 streams events 0]
        assert {[llength [lindex $res 0 1]] == 0}

        # We should fetch all the elements in the stream asking for >
        set res [r xreadgroup group mygroup myconsumer count 3 streams events >]
        assert {[llength [lindex $res 0 1]] == 3}

        # Now the history is populated with three not acked entries
        set res [r xreadgroup group mygroup myconsumer count 3 streams events 0]
        assert {[llength [lindex $res 0 1]] == 3}
    }

    test {XREADGROUP history reporting of deleted entries. Bug #5570} {
        r del mystream
        r XGROUP CREATE mystream mygroup $ MKSTREAM
        r XADD mystream 1 field1 A
        r XREADGROUP GROUP mygroup myconsumer STREAMS mystream >
        r XADD mystream MAXLEN 1 2 field1 B
        r XREADGROUP GROUP mygroup myconsumer STREAMS mystream >

        # Now we have two pending entries, however one should be deleted
        # and one should be ok (we should only see "B")
        set res [r XREADGROUP GROUP mygroup myconsumer STREAMS mystream 0-1]
        assert {[lindex $res 0 1 0] == {1-0 {}}}
        assert {[lindex $res 0 1 1] == {2-0 {field1 B}}}
    }

    test {Blocking XREADGROUP will not reply with an empty array} {
        r del mystream
        r XGROUP CREATE mystream mygroup $ MKSTREAM
        r XADD mystream 666 f v
        set res [r XREADGROUP GROUP mygroup Alice BLOCK 10 STREAMS mystream ">"]
        assert {[lindex $res 0 1 0] == {666-0 {f v}}}
        r XADD mystream 667 f2 v2
        r XDEL mystream 667
        set rd [redis_deferring_client]
        $rd XREADGROUP GROUP mygroup Alice BLOCK 10 STREAMS mystream ">"
        wait_for_blocked_clients_count 0
        assert {[$rd read] == {}} ;# before the fix, client didn't even block, but was served synchronously with {mystream {}}
        $rd close
    }

    test {Blocking XREADGROUP: key deleted} {
        r DEL mystream
        r XADD mystream 666 f v
        r XGROUP CREATE mystream mygroup $
        set rd [redis_deferring_client]
        $rd XREADGROUP GROUP mygroup Alice BLOCK 0 STREAMS mystream ">"
        wait_for_blocked_clients_count 1
        r DEL mystream
        assert_error "NOGROUP*" {$rd read}
        $rd close
    }

    test {Blocking XREADGROUP: key type changed with SET} {
        r DEL mystream
        r XADD mystream 666 f v
        r XGROUP CREATE mystream mygroup $
        set rd [redis_deferring_client]
        $rd XREADGROUP GROUP mygroup Alice BLOCK 0 STREAMS mystream ">"
        wait_for_blocked_clients_count 1
        r SET mystream val1
        assert_error "*WRONGTYPE*" {$rd read}
        $rd close
    }

    test {Blocking XREADGROUP: key type changed with transaction} {
        r DEL mystream
        r XADD mystream 666 f v
        r XGROUP CREATE mystream mygroup $
        set rd [redis_deferring_client]
        $rd XREADGROUP GROUP mygroup Alice BLOCK 0 STREAMS mystream ">"
        wait_for_blocked_clients_count 1
        r MULTI
        r DEL mystream
        r SADD mystream e1
        r EXEC
        assert_error "*WRONGTYPE*" {$rd read}
        $rd close
    }

    test {Blocking XREADGROUP: flushed DB} {
        r DEL mystream
        r XADD mystream 666 f v
        r XGROUP CREATE mystream mygroup $
        set rd [redis_deferring_client]
        $rd XREADGROUP GROUP mygroup Alice BLOCK 0 STREAMS mystream ">"
        wait_for_blocked_clients_count 1
        r FLUSHALL
        assert_error "*NOGROUP*" {$rd read}
        $rd close
    }

    test {Blocking XREADGROUP: swapped DB, key doesn't exist} {
        r SELECT 4
        r FLUSHDB
        r SELECT 9
        r DEL mystream
        r XADD mystream 666 f v
        r XGROUP CREATE mystream mygroup $
        set rd [redis_deferring_client]
        $rd SELECT 9
        $rd read
        $rd XREADGROUP GROUP mygroup Alice BLOCK 0 STREAMS mystream ">"
        wait_for_blocked_clients_count 1
        r SWAPDB 4 9
        assert_error "*NOGROUP*" {$rd read}
        $rd close
    } {0} {external:skip}

    test {Blocking XREADGROUP: swapped DB, key is not a stream} {
        r SELECT 4
        r FLUSHDB
        r LPUSH mystream e1
        r SELECT 9
        r DEL mystream
        r XADD mystream 666 f v
        r XGROUP CREATE mystream mygroup $
        set rd [redis_deferring_client]
        $rd SELECT 9
        $rd read
        $rd XREADGROUP GROUP mygroup Alice BLOCK 0 STREAMS mystream ">"
        wait_for_blocked_clients_count 1
        r SWAPDB 4 9
        assert_error "*WRONGTYPE*" {$rd read}
        $rd close
    } {0} {external:skip}

    test {XREAD and XREADGROUP against wrong parameter} {
        r DEL mystream
        r XADD mystream 666 f v
        r XGROUP CREATE mystream mygroup $
        assert_error "ERR Unbalanced 'xreadgroup' list of streams: for each stream key an ID or '>' must be specified." {r XREADGROUP GROUP mygroup Alice COUNT 1 STREAMS mystream }
        assert_error "ERR Unbalanced 'xread' list of streams: for each stream key an ID, '+', or '$' must be specified." {r XREAD COUNT 1 STREAMS mystream }
    }

    test {Blocking XREAD: key deleted} {
        r DEL mystream
        r XADD mystream 666 f v
        set rd [redis_deferring_client]
        $rd XREAD BLOCK 0 STREAMS mystream "$"
        wait_for_blocked_clients_count 1
        r DEL mystream

        r XADD mystream 667 f v
        set res [$rd read]
        assert_equal [lindex $res 0 1 0] {667-0 {f v}}
        $rd close
    }

    test {Blocking XREAD: key type changed with SET} {
        r DEL mystream
        r XADD mystream 666 f v
        set rd [redis_deferring_client]
        $rd XREAD BLOCK 0 STREAMS mystream "$"
        wait_for_blocked_clients_count 1
        r SET mystream val1

        r DEL mystream
        r XADD mystream 667 f v
        set res [$rd read]
        assert_equal [lindex $res 0 1 0] {667-0 {f v}}
        $rd close
    }

    test {Blocking XREADGROUP for stream that ran dry (issue #5299)} {
        set rd [redis_deferring_client]

        # Add a entry then delete it, now stream's last_id is 666.
        r DEL mystream
        r XGROUP CREATE mystream mygroup $ MKSTREAM
        r XADD mystream 666 key value
        r XDEL mystream 666

        # Pass a special `>` ID but without new entry, released on timeout.
        $rd XREADGROUP GROUP mygroup myconsumer BLOCK 10 STREAMS mystream >
        assert_equal [$rd read] {}

        # Throw an error if the ID equal or smaller than the last_id.
        assert_error ERR*equal*smaller* {r XADD mystream 665 key value}
        assert_error ERR*equal*smaller* {r XADD mystream 666 key value}

        # Entered blocking state and then release because of the new entry.
        $rd XREADGROUP GROUP mygroup myconsumer BLOCK 0 STREAMS mystream >
        wait_for_blocked_clients_count 1
        r XADD mystream 667 key value
        assert_equal [$rd read] {{mystream {{667-0 {key value}}}}}

        $rd close
    }

    test "Blocking XREADGROUP will ignore BLOCK if ID is not >" {
        set rd [redis_deferring_client]

        # Add a entry then delete it, now stream's last_id is 666.
        r DEL mystream
        r XGROUP CREATE mystream mygroup $ MKSTREAM
        r XADD mystream 666 key value
        r XDEL mystream 666

        # Return right away instead of blocking, return the stream with an
        # empty list instead of NIL if the ID specified is not the special `>` ID.
        foreach id {0 600 666 700} {
            $rd XREADGROUP GROUP mygroup myconsumer BLOCK 0 STREAMS mystream $id
            assert_equal [$rd read] {{mystream {}}}
        }

        # After adding a new entry, `XREADGROUP BLOCK` still return the stream
        # with an empty list because the pending list is empty.
        r XADD mystream 667 key value
        foreach id {0 600 666 667 700} {
            $rd XREADGROUP GROUP mygroup myconsumer BLOCK 0 STREAMS mystream $id
            assert_equal [$rd read] {{mystream {}}}
        }

        # After we read it once, the pending list is not empty at this time,
        # pass any ID smaller than 667 will return one of the pending entry.
        set res [r XREADGROUP GROUP mygroup myconsumer BLOCK 0 STREAMS mystream >]
        assert_equal $res {{mystream {{667-0 {key value}}}}}
        foreach id {0 600 666} {
            $rd XREADGROUP GROUP mygroup myconsumer BLOCK 0 STREAMS mystream $id
            assert_equal [$rd read] {{mystream {{667-0 {key value}}}}}
        }

        # Pass ID equal or greater than 667 will return the stream with an empty list.
        foreach id {667 700} {
            $rd XREADGROUP GROUP mygroup myconsumer BLOCK 0 STREAMS mystream $id
            assert_equal [$rd read] {{mystream {}}}
        }

        # After we ACK the pending entry, return the stream with an empty list.
        r XACK mystream mygroup 667
        foreach id {0 600 666 667 700} {
            $rd XREADGROUP GROUP mygroup myconsumer BLOCK 0 STREAMS mystream $id
            assert_equal [$rd read] {{mystream {}}}
        }

        $rd close
    }

     test {Blocking XREADGROUP for stream key that has clients blocked on list} {
        set rd [redis_deferring_client]
        set rd2 [redis_deferring_client]
        
        # First delete the stream
        r DEL mystream
        
        # now place a client blocked on non-existing key as list
        $rd2 BLPOP mystream 0
        
        # wait until we verify the client is blocked
        wait_for_blocked_clients_count 1
        
        # verify we only have 1 regular blocking key
        assert_equal 1 [getInfoProperty [r info clients] total_blocking_keys]
        assert_equal 0 [getInfoProperty [r info clients] total_blocking_keys_on_nokey]
        
        # now write mystream as stream
        r XADD mystream 666 key value
        r XGROUP CREATE mystream mygroup $ MKSTREAM
        
        # block another client on xreadgroup 
        $rd XREADGROUP GROUP mygroup myconsumer BLOCK 0 STREAMS mystream ">"
        
        # wait until we verify we have 2 blocked clients (one for the list and one for the stream)
        wait_for_blocked_clients_count 2
        
        # verify we have 1 blocking key which also have clients blocked on nokey condition
        assert_equal 1 [getInfoProperty [r info clients] total_blocking_keys]
        assert_equal 1 [getInfoProperty [r info clients] total_blocking_keys_on_nokey]

        # now delete the key and verify we have no clients blocked on nokey condition
        r DEL mystream
        assert_error "NOGROUP*" {$rd read}
        assert_equal 1 [getInfoProperty [r info clients] total_blocking_keys]
        assert_equal 0 [getInfoProperty [r info clients] total_blocking_keys_on_nokey]
        
        # close the only left client and make sure we have no more blocking keys
        $rd2 close
        
        # wait until we verify we have no more blocked clients
        wait_for_blocked_clients_count 0
        
        assert_equal 0 [getInfoProperty [r info clients] total_blocking_keys]
        assert_equal 0 [getInfoProperty [r info clients] total_blocking_keys_on_nokey]
        
        $rd close 
    }

    test {Blocking XREADGROUP for stream key that has clients blocked on stream - avoid endless loop} {
        r DEL mystream
        r XGROUP CREATE mystream mygroup $ MKSTREAM

        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]
        set rd3 [redis_deferring_client]

        $rd1 xreadgroup GROUP mygroup myuser COUNT 10 BLOCK 10000 STREAMS mystream >
        $rd2 xreadgroup GROUP mygroup myuser COUNT 10 BLOCK 10000 STREAMS mystream >
        $rd3 xreadgroup GROUP mygroup myuser COUNT 10 BLOCK 10000 STREAMS mystream >

        wait_for_blocked_clients_count 3

        r xadd mystream MAXLEN 5000 * field1 value1 field2 value2 field3 value3

        $rd1 close
        $rd2 close
        $rd3 close

        assert_equal [r ping] {PONG}
    }

    test {Blocking XREADGROUP for stream key that has clients blocked on stream - reprocessing command} {
        r DEL mystream
        r XGROUP CREATE mystream mygroup $ MKSTREAM

        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]

        $rd1 xreadgroup GROUP mygroup myuser BLOCK 0 STREAMS mystream >
        wait_for_blocked_clients_count 1

        set start [clock milliseconds]
        $rd2 xreadgroup GROUP mygroup myuser BLOCK 1000 STREAMS mystream >
        wait_for_blocked_clients_count 2

        # After a while call xadd and let rd2 re-process the command.
        after 200
        r xadd mystream * field value
        assert_equal {} [$rd2 read]
        set end [clock milliseconds]

        # Before the fix in #13004, this time would have been 1200+ (i.e. more than 1200ms),
        # now it should be 1000, but in order to avoid timing issues, we increase the range a bit.
        assert_range [expr $end-$start] 1000 1150

        $rd1 close
        $rd2 close
    }

    test {XGROUP DESTROY should unblock XREADGROUP with -NOGROUP} {
        r config resetstat
        r del mystream
        r XGROUP CREATE mystream mygroup $ MKSTREAM
        set rd [redis_deferring_client]
        $rd XREADGROUP GROUP mygroup Alice BLOCK 0 STREAMS mystream ">"
        wait_for_blocked_clients_count 1
        r XGROUP DESTROY mystream mygroup
        assert_error "NOGROUP*" {$rd read}
        $rd close

        # verify command stats, error stats and error counter work on failed blocked command
        assert_match {*count=1*} [errorrstat NOGROUP r]
        assert_match {*calls=1,*,rejected_calls=0,failed_calls=1*} [cmdrstat xreadgroup r]
        assert_equal [s total_error_replies] 1
    }

    test {XGROUP DESTROY removes all consumer group references} {
        r DEL mystream
        for {set j 0} {$j < 5} {incr j} {
            r XADD mystream $j-1 item $j
        }

        r XGROUP CREATE mystream mygroup 0
        r XREADGROUP GROUP mygroup consumer1 STREAMS mystream >
        assert {[lindex [r XPENDING mystream mygroup] 0] == 5}

        # Try to delete a message with ACKED - should fail because both groups have references
        assert_equal {2 2 2 2 2} [r XDELEX mystream ACKED IDS 5 0-1 1-1 2-1 3-1 4-1]

        # Destroy one consumer group, and then we can delete all the entries with ACKED.
        r XGROUP DESTROY mystream mygroup
        assert_equal {1 1 1 1 1} [r XDELEX mystream ACKED IDS 5 0-1 1-1 2-1 3-1 4-1]
        assert_equal 0 [r XLEN mystream] 
    }

    test {XGROUP DESTROY correctly manage min_cgroup_last_id cache} {
        r DEL mystream
        # Add some entries
        r XADD mystream 1-0 f1 v1
        r XADD mystream 2-0 f2 v2
        r XADD mystream 3-0 f3 v3
        r XADD mystream 4-0 f4 v4
        r XADD mystream 5-0 f5 v5

        # Create two consumer groups
        r XGROUP CREATE mystream group1 1-0 ;# min_cgroup_last_id is 1-0 now
        r XGROUP CREATE mystream group2 3-0

        # Entry 1-0 should be deletable (1-0 <= min_cgroup_last_id and not in any PEL)
        assert_equal {1} [r XDELEX mystream ACKED IDS 1 1-0]

        # Entry 2-0 should be referenced (2-0 > 1-0, not yet consumed by all consume groups)
        assert_equal {2} [r XDELEX mystream ACKED IDS 1 2-0]

        # Destroy group1
        # min_cgroup_last_id is 3-0 now
        r XGROUP DESTROY mystream group1

        # Entry 2-0 should now be deletable (2-0 < 3-0 and not in any PEL)
        assert_equal {1} [r XDELEX mystream ACKED IDS 1 2-0]
    }

    test {RENAME can unblock XREADGROUP with data} {
        r del mystream{t}
        r XGROUP CREATE mystream{t} mygroup $ MKSTREAM
        set rd [redis_deferring_client]
        $rd XREADGROUP GROUP mygroup Alice BLOCK 0 STREAMS mystream{t} ">"
        wait_for_blocked_clients_count 1
        r XGROUP CREATE mystream2{t} mygroup $ MKSTREAM
        r XADD mystream2{t} 100 f1 v1
        r RENAME mystream2{t} mystream{t}
        assert_equal "{mystream{t} {{100-0 {f1 v1}}}}" [$rd read] ;# mystream2{t} had mygroup before RENAME
        $rd close
    }

    test {RENAME can unblock XREADGROUP with -NOGROUP} {
        r del mystream{t}
        r XGROUP CREATE mystream{t} mygroup $ MKSTREAM
        set rd [redis_deferring_client]
        $rd XREADGROUP GROUP mygroup Alice BLOCK 0 STREAMS mystream{t} ">"
        wait_for_blocked_clients_count 1
        r XADD mystream2{t} 100 f1 v1
        r RENAME mystream2{t} mystream{t}
        assert_error "*NOGROUP*" {$rd read} ;# mystream2{t} didn't have mygroup before RENAME
        $rd close
    }

    test {XCLAIM can claim PEL items from another consumer} {
        # Add 3 items into the stream, and create a consumer group
        r del mystream
        set id1 [r XADD mystream * a 1]
        set id2 [r XADD mystream * b 2]
        set id3 [r XADD mystream * c 3]
        r XGROUP CREATE mystream mygroup 0

        # Consumer 1 reads item 1 from the stream without acknowledgements.
        # Consumer 2 then claims pending item 1 from the PEL of consumer 1
        set reply [
            r XREADGROUP GROUP mygroup consumer1 count 1 STREAMS mystream >
        ]
        assert {[llength [lindex $reply 0 1 0 1]] == 2}
        assert {[lindex $reply 0 1 0 1] eq {a 1}}

        # make sure the entry is present in both the group, and the right consumer
        assert {[llength [r XPENDING mystream mygroup - + 10]] == 1}
        assert {[llength [r XPENDING mystream mygroup - + 10 consumer1]] == 1}
        assert {[llength [r XPENDING mystream mygroup - + 10 consumer2]] == 0}

        after 200
        set reply [
            r XCLAIM mystream mygroup consumer2 10 $id1
        ]
        assert {[llength [lindex $reply 0 1]] == 2}
        assert {[lindex $reply 0 1] eq {a 1}}

        # make sure the entry is present in both the group, and the right consumer
        assert {[llength [r XPENDING mystream mygroup - + 10]] == 1}
        assert {[llength [r XPENDING mystream mygroup - + 10 consumer1]] == 0}
        assert {[llength [r XPENDING mystream mygroup - + 10 consumer2]] == 1}

        # Consumer 1 reads another 2 items from stream
        r XREADGROUP GROUP mygroup consumer1 count 2 STREAMS mystream >
        after 200

        # Delete item 2 from the stream. Now consumer 1 has PEL that contains
        # only item 3. Try to use consumer 2 to claim the deleted item 2
        # from the PEL of consumer 1, this should be NOP
        r XDEL mystream $id2
        set reply [
            r XCLAIM mystream mygroup consumer2 10 $id2
        ]
        assert {[llength $reply] == 0}

        # Delete item 3 from the stream. Now consumer 1 has PEL that is empty.
        # Try to use consumer 2 to claim the deleted item 3 from the PEL
        # of consumer 1, this should be NOP
        after 200
        r XDEL mystream $id3
        set reply [
            r XCLAIM mystream mygroup consumer2 10 $id3
        ]
        assert {[llength $reply] == 0}
    }

    test {XCLAIM without JUSTID increments delivery count} {
        # Add 3 items into the stream, and create a consumer group
        r del mystream
        set id1 [r XADD mystream * a 1]
        set id2 [r XADD mystream * b 2]
        set id3 [r XADD mystream * c 3]
        r XGROUP CREATE mystream mygroup 0

        # Consumer 1 reads item 1 from the stream without acknowledgements.
        # Consumer 2 then claims pending item 1 from the PEL of consumer 1
        set reply [
            r XREADGROUP GROUP mygroup consumer1 count 1 STREAMS mystream >
        ]
        assert {[llength [lindex $reply 0 1 0 1]] == 2}
        assert {[lindex $reply 0 1 0 1] eq {a 1}}
        after 200
        set reply [
            r XCLAIM mystream mygroup consumer2 10 $id1
        ]
        assert {[llength [lindex $reply 0 1]] == 2}
        assert {[lindex $reply 0 1] eq {a 1}}

        set reply [
            r XPENDING mystream mygroup - + 10
        ]
        assert {[llength [lindex $reply 0]] == 4}
        assert {[lindex $reply 0 3] == 2}

        # Consumer 3 then claims pending item 1 from the PEL of consumer 2 using JUSTID
        after 200
        set reply [
            r XCLAIM mystream mygroup consumer3 10 $id1 JUSTID
        ]
        assert {[llength $reply] == 1}
        assert {[lindex $reply 0] eq $id1}

        set reply [
            r XPENDING mystream mygroup - + 10
        ]
        assert {[llength [lindex $reply 0]] == 4}
        assert {[lindex $reply 0 3] == 2}
    }

    test {XCLAIM same consumer} {
        # Add 3 items into the stream, and create a consumer group
        r del mystream
        set id1 [r XADD mystream * a 1]
        set id2 [r XADD mystream * b 2]
        set id3 [r XADD mystream * c 3]
        r XGROUP CREATE mystream mygroup 0

        set reply [r XREADGROUP GROUP mygroup consumer1 count 1 STREAMS mystream >]
        assert {[llength [lindex $reply 0 1 0 1]] == 2}
        assert {[lindex $reply 0 1 0 1] eq {a 1}}
        after 200
        # re-claim with the same consumer that already has it
        assert {[llength [r XCLAIM mystream mygroup consumer1 10 $id1]] == 1}

        # make sure the entry is still in the PEL
        set reply [r XPENDING mystream mygroup - + 10]
        assert {[llength $reply] == 1}
        assert {[lindex $reply 0 1] eq {consumer1}}
    }

    test {XAUTOCLAIM can claim PEL items from another consumer} {
        # Add 3 items into the stream, and create a consumer group
        r del mystream
        set id1 [r XADD mystream * a 1]
        set id2 [r XADD mystream * b 2]
        set id3 [r XADD mystream * c 3]
        set id4 [r XADD mystream * d 4]
        r XGROUP CREATE mystream mygroup 0

        # Consumer 1 reads item 1 from the stream without acknowledgements.
        # Consumer 2 then claims pending item 1 from the PEL of consumer 1
        set reply [r XREADGROUP GROUP mygroup consumer1 count 1 STREAMS mystream >]
        assert_equal [llength [lindex $reply 0 1 0 1]] 2
        assert_equal [lindex $reply 0 1 0 1] {a 1}
        after 200
        set reply [r XAUTOCLAIM mystream mygroup consumer2 10 - COUNT 1]
        assert_equal [llength $reply] 3
        assert_equal [lindex $reply 0] "0-0"
        assert_equal [llength [lindex $reply 1]] 1
        assert_equal [llength [lindex $reply 1 0]] 2
        assert_equal [llength [lindex $reply 1 0 1]] 2
        assert_equal [lindex $reply 1 0 1] {a 1}

        # Consumer 1 reads another 2 items from stream
        r XREADGROUP GROUP mygroup consumer1 count 3 STREAMS mystream >

        # For min-idle-time
        after 200

        # Delete item 2 from the stream. Now consumer 1 has PEL that contains
        # only item 3. Try to use consumer 2 to claim the deleted item 2
        # from the PEL of consumer 1, this should return nil
        r XDEL mystream $id2

        # id1 and id3 are self-claimed here but not id2 ('count' was set to 3)
        # we make sure id2 is indeed skipped (the cursor points to id4)
        set reply [r XAUTOCLAIM mystream mygroup consumer2 10 - COUNT 3]

        assert_equal [llength $reply] 3
        assert_equal [lindex $reply 0] $id4
        assert_equal [llength [lindex $reply 1]] 2
        assert_equal [llength [lindex $reply 1 0]] 2
        assert_equal [llength [lindex $reply 1 0 1]] 2
        assert_equal [lindex $reply 1 0 1] {a 1}
        assert_equal [lindex $reply 1 1 1] {c 3}
        assert_equal [llength [lindex $reply 2]] 1
        assert_equal [llength [lindex $reply 2 0]] 1

        # Delete item 3 from the stream. Now consumer 1 has PEL that is empty.
        # Try to use consumer 2 to claim the deleted item 3 from the PEL
        # of consumer 1, this should return nil
        after 200

        r XDEL mystream $id4

        # id1 and id3 are self-claimed here but not id2 and id4 ('count' is default 100)
        set reply [r XAUTOCLAIM mystream mygroup consumer2 10 - JUSTID]

        # we also test the JUSTID modifier here. note that, when using JUSTID,
        # deleted entries are returned in reply (consistent with XCLAIM).

        assert_equal [llength $reply] 3
        assert_equal [lindex $reply 0] {0-0}
        assert_equal [llength [lindex $reply 1]] 2
        assert_equal [lindex $reply 1 0] $id1
        assert_equal [lindex $reply 1 1] $id3
    }

    test {XAUTOCLAIM as an iterator} {
        # Add 5 items into the stream, and create a consumer group
        r del mystream
        set id1 [r XADD mystream * a 1]
        set id2 [r XADD mystream * b 2]
        set id3 [r XADD mystream * c 3]
        set id4 [r XADD mystream * d 4]
        set id5 [r XADD mystream * e 5]
        r XGROUP CREATE mystream mygroup 0

        # Read 5 messages into consumer1
        r XREADGROUP GROUP mygroup consumer1 count 90 STREAMS mystream >

        # For min-idle-time
        after 200

        # Claim 2 entries
        set reply [r XAUTOCLAIM mystream mygroup consumer2 10 - COUNT 2]
        assert_equal [llength $reply] 3
        set cursor [lindex $reply 0]
        assert_equal $cursor $id3
        assert_equal [llength [lindex $reply 1]] 2
        assert_equal [llength [lindex $reply 1 0 1]] 2
        assert_equal [lindex $reply 1 0 1] {a 1}

        # Claim 2 more entries
        set reply [r XAUTOCLAIM mystream mygroup consumer2 10 $cursor COUNT 2]
        assert_equal [llength $reply] 3
        set cursor [lindex $reply 0]
        assert_equal $cursor $id5
        assert_equal [llength [lindex $reply 1]] 2
        assert_equal [llength [lindex $reply 1 0 1]] 2
        assert_equal [lindex $reply 1 0 1] {c 3}

        # Claim last entry
        set reply [r XAUTOCLAIM mystream mygroup consumer2 10 $cursor COUNT 1]
        assert_equal [llength $reply] 3
        set cursor [lindex $reply 0]
        assert_equal $cursor {0-0}
        assert_equal [llength [lindex $reply 1]] 1
        assert_equal [llength [lindex $reply 1 0 1]] 2
        assert_equal [lindex $reply 1 0 1] {e 5}
    }

    test {XAUTOCLAIM COUNT must be > 0} {
       assert_error "ERR COUNT must be > 0" {r XAUTOCLAIM key group consumer 1 1 COUNT 0}
    }

    test {XCLAIM with XDEL} {
        r DEL x
        r XADD x 1-0 f v
        r XADD x 2-0 f v
        r XADD x 3-0 f v
        r XGROUP CREATE x grp 0
        assert_equal [r XREADGROUP GROUP grp Alice STREAMS x >] {{x {{1-0 {f v}} {2-0 {f v}} {3-0 {f v}}}}}
        r XDEL x 2-0
        assert_equal [r XCLAIM x grp Bob 0 1-0 2-0 3-0] {{1-0 {f v}} {3-0 {f v}}}
        assert_equal [r XPENDING x grp - + 10 Alice] {}
    }

    test {XCLAIM with trimming} {
        r DEL x
        r config set stream-node-max-entries 2
        r XADD x 1-0 f v
        r XADD x 2-0 f v
        r XADD x 3-0 f v
        r XGROUP CREATE x grp 0
        assert_equal [r XREADGROUP GROUP grp Alice STREAMS x >] {{x {{1-0 {f v}} {2-0 {f v}} {3-0 {f v}}}}}
        r XTRIM x MAXLEN 1
        assert_equal [r XCLAIM x grp Bob 0 1-0 2-0 3-0] {{3-0 {f v}}}
        assert_equal [r XPENDING x grp - + 10 Alice] {}
    }

    test {XAUTOCLAIM with XDEL} {
        r DEL x
        r XADD x 1-0 f v
        r XADD x 2-0 f v
        r XADD x 3-0 f v
        r XGROUP CREATE x grp 0
        assert_equal [r XREADGROUP GROUP grp Alice STREAMS x >] {{x {{1-0 {f v}} {2-0 {f v}} {3-0 {f v}}}}}
        r XDEL x 2-0
        assert_equal [r XAUTOCLAIM x grp Bob 0 0-0] {0-0 {{1-0 {f v}} {3-0 {f v}}} 2-0}
        assert_equal [r XPENDING x grp - + 10 Alice] {}
    }

    test {XAUTOCLAIM with XDEL and count} {
        r DEL x
        r XADD x 1-0 f v
        r XADD x 2-0 f v
        r XADD x 3-0 f v
        r XGROUP CREATE x grp 0
        assert_equal [r XREADGROUP GROUP grp Alice STREAMS x >] {{x {{1-0 {f v}} {2-0 {f v}} {3-0 {f v}}}}}
        r XDEL x 1-0
        r XDEL x 2-0
        assert_equal [r XAUTOCLAIM x grp Bob 0 0-0 COUNT 1] {2-0 {} 1-0}
        assert_equal [r XAUTOCLAIM x grp Bob 0 2-0 COUNT 1] {3-0 {} 2-0}
        assert_equal [r XAUTOCLAIM x grp Bob 0 3-0 COUNT 1] {0-0 {{3-0 {f v}}} {}}
        assert_equal [r XPENDING x grp - + 10 Alice] {}
    }

    test {XAUTOCLAIM with out of range count} {
        assert_error {ERR COUNT*} {r XAUTOCLAIM x grp Bob 0 3-0 COUNT 8070450532247928833}
    }

    test {XCLAIM with trimming} {
        r DEL x
        r config set stream-node-max-entries 2
        r XADD x 1-0 f v
        r XADD x 2-0 f v
        r XADD x 3-0 f v
        r XGROUP CREATE x grp 0
        assert_equal [r XREADGROUP GROUP grp Alice STREAMS x >] {{x {{1-0 {f v}} {2-0 {f v}} {3-0 {f v}}}}}
        r XTRIM x MAXLEN 1
        assert_equal [r XAUTOCLAIM x grp Bob 0 0-0] {0-0 {{3-0 {f v}}} {1-0 2-0}}
        assert_equal [r XPENDING x grp - + 10 Alice] {}
    }

    test {XINFO FULL output} {
        r del x
        r XADD x 100 a 1
        r XADD x 101 b 1
        r XADD x 102 c 1
        r XADD x 103 e 1
        r XADD x 104 f 1
        r XGROUP CREATE x g1 0
        r XGROUP CREATE x g2 0
        r XREADGROUP GROUP g1 Alice COUNT 1 STREAMS x >
        r XREADGROUP GROUP g1 Bob COUNT 1 STREAMS x >
        r XREADGROUP GROUP g1 Bob NOACK COUNT 1 STREAMS x >
        r XREADGROUP GROUP g2 Charlie COUNT 4 STREAMS x >
        r XDEL x 103

        set reply [r XINFO STREAM x FULL]
        assert_equal [llength $reply] 30
        assert_equal [dict get $reply length] 4
        assert_equal [dict get $reply entries] "{100-0 {a 1}} {101-0 {b 1}} {102-0 {c 1}} {104-0 {f 1}}"

        # First consumer group
        set group [lindex [dict get $reply groups] 0]
        assert_equal [dict get $group name] "g1"
        assert_equal [lindex [dict get $group pending] 0 0] "100-0"
        set consumer [lindex [dict get $group consumers] 0]
        assert_equal [dict get $consumer name] "Alice"
        assert_equal [lindex [dict get $consumer pending] 0 0] "100-0" ;# first entry in first consumer's PEL

        # Second consumer group
        set group [lindex [dict get $reply groups] 1]
        assert_equal [dict get $group name] "g2"
        set consumer [lindex [dict get $group consumers] 0]
        assert_equal [dict get $consumer name] "Charlie"
        assert_equal [lindex [dict get $consumer pending] 0 0] "100-0" ;# first entry in first consumer's PEL
        assert_equal [lindex [dict get $consumer pending] 1 0] "101-0" ;# second entry in first consumer's PEL

        set reply [r XINFO STREAM x FULL COUNT 1]
        assert_equal [llength $reply] 30
        assert_equal [dict get $reply length] 4
        assert_equal [dict get $reply entries] "{100-0 {a 1}}"
    }

    test {Consumer seen-time and active-time} {
        r DEL mystream
        r XGROUP CREATE mystream mygroup $ MKSTREAM
        r XREADGROUP GROUP mygroup Alice COUNT 1 STREAMS mystream >
        after 100
        set reply [r xinfo consumers mystream mygroup]
        set consumer_info [lindex $reply 0]
        assert {[dict get $consumer_info idle] >= 100} ;# consumer idle (seen-time)
        assert_equal [dict get $consumer_info inactive] "-1" ;# consumer inactive (active-time)

        r XADD mystream * f v
        r XREADGROUP GROUP mygroup Alice COUNT 1 STREAMS mystream >
        set reply [r xinfo consumers mystream mygroup]
        set consumer_info [lindex $reply 0]
        assert_equal [lindex $consumer_info 1] "Alice" ;# consumer name
        assert {[dict get $consumer_info idle] < 80} ;# consumer idle (seen-time)
        assert {[dict get $consumer_info inactive] < 80} ;# consumer inactive (active-time)

        after 100
        r XREADGROUP GROUP mygroup Alice COUNT 1 STREAMS mystream >
        set reply [r xinfo consumers mystream mygroup]
        set consumer_info [lindex $reply 0]
        assert {[dict get $consumer_info idle] < 80} ;# consumer idle (seen-time)
        assert {[dict get $consumer_info inactive] >= 100} ;# consumer inactive (active-time)


        # Simulate loading from RDB

        set reply [r XINFO STREAM x FULL]
        set group [lindex [dict get $reply groups] 0]
        set consumer [lindex [dict get $group consumers] 0]
        set prev_seen [dict get $consumer seen-time]
        set prev_active [dict get $consumer active-time]

        set dump [r DUMP mystream]
        r DEL mystream
        r RESTORE mystream 0 $dump

        set reply [r XINFO STREAM x FULL]
        set group [lindex [dict get $reply groups] 0]
        set consumer [lindex [dict get $group consumers] 0]
        assert_equal $prev_seen [dict get $consumer seen-time]
        assert_equal $prev_active [dict get $consumer active-time]
    }

    test {XGROUP CREATECONSUMER: create consumer if does not exist} {
        r del mystream
        r XGROUP CREATE mystream mygroup $ MKSTREAM
        r XADD mystream * f v

        set reply [r xinfo groups mystream]
        set group_info [lindex $reply 0]
        set n_consumers [lindex $group_info 3]
        assert_equal $n_consumers 0 ;# consumers number in cg

        # create consumer using XREADGROUP
        r XREADGROUP GROUP mygroup Alice COUNT 1 STREAMS mystream >

        set reply [r xinfo groups mystream]
        set group_info [lindex $reply 0]
        set n_consumers [lindex $group_info 3]
        assert_equal $n_consumers 1 ;# consumers number in cg

        set reply [r xinfo consumers mystream mygroup]
        set consumer_info [lindex $reply 0]
        assert_equal [lindex $consumer_info 1] "Alice" ;# consumer name

        # create group using XGROUP CREATECONSUMER when Alice already exists
        set created [r XGROUP CREATECONSUMER mystream mygroup Alice]
        assert_equal $created 0

        # create group using XGROUP CREATECONSUMER when Bob does not exist
        set created [r XGROUP CREATECONSUMER mystream mygroup Bob]
        assert_equal $created 1

        set reply [r xinfo groups mystream]
        set group_info [lindex $reply 0]
        set n_consumers [lindex $group_info 3]
        assert_equal $n_consumers 2 ;# consumers number in cg

        set reply [r xinfo consumers mystream mygroup]
        set consumer_info [lindex $reply 0]
        assert_equal [lindex $consumer_info 1] "Alice" ;# consumer name
        set consumer_info [lindex $reply 1]
        assert_equal [lindex $consumer_info 1] "Bob" ;# consumer name
    }

    test {XGROUP CREATECONSUMER: group must exist} {
        r del mystream
        r XADD mystream * f v
        assert_error "*NOGROUP*" {r XGROUP CREATECONSUMER mystream mygroup consumer}
    }

    test {XREADGROUP of multiple entries changes dirty by one} {
        r DEL x
        r XADD x 1-0 data a
        r XADD x 2-0 data b
        r XADD x 3-0 data c
        r XADD x 4-0 data d
        r XGROUP CREATE x g1 0
        r XGROUP CREATECONSUMER x g1 Alice

        set dirty [s rdb_changes_since_last_save]
        set res [r XREADGROUP GROUP g1 Alice COUNT 2 STREAMS x ">"]
        assert_equal $res {{x {{1-0 {data a}} {2-0 {data b}}}}}
        set dirty2 [s rdb_changes_since_last_save]
        assert {$dirty2 == $dirty + 1}

        set dirty [s rdb_changes_since_last_save]
        set res [r XREADGROUP GROUP g1 Alice NOACK COUNT 2 STREAMS x ">"]
        assert_equal $res {{x {{3-0 {data c}} {4-0 {data d}}}}}
        set dirty2 [s rdb_changes_since_last_save]
        assert {$dirty2 == $dirty + 1}
    }

    test {XREADGROUP from PEL does not change dirty} {
        # Techinally speaking, XREADGROUP from PEL should cause propagation
        # because it change the delivery count/time
        # It was decided that this metadata changes are too insiginificant
        # to justify propagation
        # This test covers that.
        r DEL x
        r XADD x 1-0 data a
        r XADD x 2-0 data b
        r XADD x 3-0 data c
        r XADD x 4-0 data d
        r XGROUP CREATE x g1 0
        r XGROUP CREATECONSUMER x g1 Alice

        set res [r XREADGROUP GROUP g1 Alice COUNT 2 STREAMS x ">"]
        assert_equal $res {{x {{1-0 {data a}} {2-0 {data b}}}}}

        set dirty [s rdb_changes_since_last_save]
        set res [r XREADGROUP GROUP g1 Alice COUNT 2 STREAMS x 0]
        assert_equal $res {{x {{1-0 {data a}} {2-0 {data b}}}}}
        set dirty2 [s rdb_changes_since_last_save]
        assert {$dirty2 == $dirty}

        set dirty [s rdb_changes_since_last_save]
        set res [r XREADGROUP GROUP g1 Alice COUNT 2 STREAMS x 9000]
        assert_equal $res {{x {}}}
        set dirty2 [s rdb_changes_since_last_save]
        assert {$dirty2 == $dirty}

        # The current behavior is that we create the consumer (causes dirty++) even
        # if we onlyneed to read from PEL.
        # It feels like we shouldn't create the consumer in that case, but I added
        # this test just for coverage of current behavior
        set dirty [s rdb_changes_since_last_save]
        set res [r XREADGROUP GROUP g1 noconsumer COUNT 2 STREAMS x 0]
        assert_equal $res {{x {}}}
        set dirty2 [s rdb_changes_since_last_save]
        assert {$dirty2 == $dirty + 1}
    }

    start_server {tags {"stream needs:debug"} overrides {appendonly yes aof-use-rdb-preamble no appendfsync always}} {
        test {XREADGROUP with NOACK creates consumer} {
            r del mystream
            r XGROUP CREATE mystream mygroup $ MKSTREAM
            r XADD mystream * f1 v1
            r XREADGROUP GROUP mygroup Alice NOACK STREAMS mystream ">"
            set rd [redis_deferring_client]
            $rd XREADGROUP GROUP mygroup Bob BLOCK 0 NOACK STREAMS mystream ">"
            wait_for_blocked_clients_count 1
            r XADD mystream * f2 v2
            set grpinfo [r xinfo groups mystream]

            r debug loadaof
            assert_equal [r xinfo groups mystream] $grpinfo
            set reply [r xinfo consumers mystream mygroup]
            set consumer_info [lindex $reply 0]
            assert_equal [lindex $consumer_info 1] "Alice" ;# consumer name
            set consumer_info [lindex $reply 1]
            assert_equal [lindex $consumer_info 1] "Bob" ;# consumer name
            $rd close
        }

        test {Consumer without PEL is present in AOF after AOFRW} {
            r del mystream
            r XGROUP CREATE mystream mygroup $ MKSTREAM
            r XADD mystream * f v
            r XREADGROUP GROUP mygroup Alice NOACK STREAMS mystream ">"
            set rd [redis_deferring_client]
            $rd XREADGROUP GROUP mygroup Bob BLOCK 0 NOACK STREAMS mystream ">"
            wait_for_blocked_clients_count 1
            r XGROUP CREATECONSUMER mystream mygroup Charlie
            set grpinfo [lindex [r xinfo groups mystream] 0]

            r bgrewriteaof
            waitForBgrewriteaof r
            r debug loadaof

            set curr_grpinfo [lindex [r xinfo groups mystream] 0]
            assert {$curr_grpinfo == $grpinfo}
            set n_consumers [lindex $grpinfo 3]

            # All consumers are created via XREADGROUP, regardless of whether they managed
            # to read any entries ot not
            assert_equal $n_consumers 3
            $rd close
        }
    }

    test {Consumer group read counter and lag in empty streams} {
        r DEL x
        r XGROUP CREATE x g1 0 MKSTREAM

        set reply [r XINFO STREAM x FULL]
        set group [lindex [dict get $reply groups] 0]
        assert_equal [dict get $reply max-deleted-entry-id] "0-0"
        assert_equal [dict get $reply entries-added] 0
        assert_equal [dict get $group entries-read] {}
        assert_equal [dict get $group lag] 0

        r XADD x 1-0 data a
        r XDEL x 1-0

        set reply [r XINFO STREAM x FULL]
        set group [lindex [dict get $reply groups] 0]
        assert_equal [dict get $reply max-deleted-entry-id] "1-0"
        assert_equal [dict get $reply entries-added] 1
        assert_equal [dict get $group entries-read] {}
        assert_equal [dict get $group lag] 0
    }

    test {Consumer group read counter and lag sanity} {
        r DEL x
        r XADD x 1-0 data a
        r XADD x 2-0 data b
        r XADD x 3-0 data c
        r XADD x 4-0 data d
        r XADD x 5-0 data e
        r XGROUP CREATE x g1 0

        set reply [r XINFO STREAM x FULL]
        set group [lindex [dict get $reply groups] 0]
        assert_equal [dict get $group entries-read] {}
        assert_equal [dict get $group lag] 5

        r XREADGROUP GROUP g1 c11 COUNT 1 STREAMS x >
        set reply [r XINFO STREAM x FULL]
        set group [lindex [dict get $reply groups] 0]
        assert_equal [dict get $group entries-read] 1
        assert_equal [dict get $group lag] 4

        r XREADGROUP GROUP g1 c12 COUNT 10 STREAMS x >
        set reply [r XINFO STREAM x FULL]
        set group [lindex [dict get $reply groups] 0]
        assert_equal [dict get $group entries-read] 5
        assert_equal [dict get $group lag] 0

        r XADD x 6-0 data f
        set reply [r XINFO STREAM x FULL]
        set group [lindex [dict get $reply groups] 0]
        assert_equal [dict get $group entries-read] 5
        assert_equal [dict get $group lag] 1
    }

    test {Consumer group lag with XDELs} {
        r DEL x
        r XADD x 1-0 data a
        r XADD x 2-0 data b
        r XADD x 3-0 data c
        r XADD x 4-0 data d
        r XADD x 5-0 data e
        r XDEL x 3-0
        r XGROUP CREATE x g1 0
        r XGROUP CREATE x g2 0

        set reply [r XINFO STREAM x FULL]
        set group [lindex [dict get $reply groups] 0]
        assert_equal [dict get $group entries-read] {}
        assert_equal [dict get $group lag] {}

        r XREADGROUP GROUP g1 c11 COUNT 1 STREAMS x >
        set reply [r XINFO STREAM x FULL]
        set group [lindex [dict get $reply groups] 0]
        assert_equal [dict get $group entries-read] {}
        assert_equal [dict get $group lag] {}

        r XREADGROUP GROUP g1 c11 COUNT 1 STREAMS x >
        set reply [r XINFO STREAM x FULL]
        set group [lindex [dict get $reply groups] 0]
        assert_equal [dict get $group entries-read] {}
        assert_equal [dict get $group lag] {}

        r XREADGROUP GROUP g1 c11 COUNT 1 STREAMS x >
        set reply [r XINFO STREAM x FULL]
        set group [lindex [dict get $reply groups] 0]
        assert_equal [dict get $group entries-read] {}
        assert_equal [dict get $group lag] {}

        r XREADGROUP GROUP g1 c11 COUNT 1 STREAMS x >
        set reply [r XINFO STREAM x FULL]
        set group [lindex [dict get $reply groups] 0]
        assert_equal [dict get $group entries-read] 5
        assert_equal [dict get $group lag] 0

        r XADD x 6-0 data f
        set reply [r XINFO STREAM x FULL]
        set group [lindex [dict get $reply groups] 0]
        assert_equal [dict get $group entries-read] 5
        assert_equal [dict get $group lag] 1

        r XTRIM x MINID = 3-0
        set reply [r XINFO STREAM x FULL]
        set group [lindex [dict get $reply groups] 0]
        assert_equal [dict get $group entries-read] 5
        assert_equal [dict get $group lag] 1
        set group [lindex [dict get $reply groups] 1]
        assert_equal [dict get $group entries-read] {}
        assert_equal [dict get $group lag] 3

        r XTRIM x MINID = 5-0
        set reply [r XINFO STREAM x FULL]
        set group [lindex [dict get $reply groups] 0]
        assert_equal [dict get $group entries-read] 5
        assert_equal [dict get $group lag] 1
        set group [lindex [dict get $reply groups] 1]
        assert_equal [dict get $group entries-read] {}
        assert_equal [dict get $group lag] 2
    }

    test {Consumer Group Lag with XDELs and tombstone after the last_id of consume group} {
        r DEL x
        r XGROUP CREATE x g1 $ MKSTREAM
        r XADD x 1-0 data a
        r XREADGROUP GROUP g1 alice STREAMS x > ;# Read one entry
        r XADD x 2-0 data c
        r XADD x 3-0 data d
        r XDEL x 2-0

        # Now the latest tombstone(2-0) is before the first entry(3-0), but there is still
        # a tombstone(2-0) after the last_id(1-0) of the consume group.
        set reply [r XINFO STREAM x FULL]
        set group [lindex [dict get $reply groups] 0]
        assert_equal [dict get $group entries-read] 1
        assert_equal [dict get $group lag] {}

        r XDEL x 1-0
        # Although there is a tombstone(2-0) after the consumer group's last_id(1-0), all
        # entries before the maximal tombstone have been deleted. This means that both the
        # last_id and the largest tombstone are behind the first entry. Therefore, tombstones
        # no longer affect the lag, which now reflects the remaining entries in the stream.
        set reply [r XINFO STREAM x FULL]
        set group [lindex [dict get $reply groups] 0]
        assert_equal [dict get $group entries-read] 1
        assert_equal [dict get $group lag] 1

        # Now there is a tombstone(2-0) after the last_id of the consume group, so after consuming
        # entry(3-0), the group's counter will be invalid.
        r XREADGROUP GROUP g1 alice STREAMS x > 
        set reply [r XINFO STREAM x FULL]
        set group [lindex [dict get $reply groups] 0]
        assert_equal [dict get $group entries-read] 3
        assert_equal [dict get $group lag] 0
    }

    test {Consumer group lag with XTRIM} {
        r DEL x
        r XGROUP CREATE x mygroup $ MKSTREAM
        r XADD x 1-0 data a
        r XADD x 2-0 data b
        r XADD x 3-0 data c
        r XADD x 4-0 data d
        r XADD x 5-0 data e
        r XREADGROUP GROUP mygroup alice COUNT 1 STREAMS x >

        set reply [r XINFO STREAM x FULL]
        set group [lindex [dict get $reply groups] 0]
        assert_equal [dict get $group entries-read] 1
        assert_equal [dict get $group lag] 4

        # Although XTRIM doesn't update the `max-deleted-entry-id`, it always updates the
        # position of the first entry. When trimming causes the first entry to be behind
        # the consumer group's last_id, the consumer group's lag will always be equal to
        # the number of remainin entries in the stream.
        r XTRIM x MAXLEN 1
        set reply [r XINFO STREAM x FULL]
        set group [lindex [dict get $reply groups] 0]
        assert_equal [dict get $reply max-deleted-entry-id] "0-0"
        assert_equal [dict get $group entries-read] 1
        assert_equal [dict get $group lag] 1

        # When all the entries are read, the lag is always 0.
        r XREADGROUP GROUP mygroup alice STREAMS x >
        set reply [r XINFO STREAM x FULL]
        set group [lindex [dict get $reply groups] 0]
        assert_equal [dict get $group entries-read] 5
        assert_equal [dict get $group lag] 0

        r XADD x 6-0 data f
        set reply [r XINFO STREAM x FULL]
        set group [lindex [dict get $reply groups] 0]
        assert_equal [dict get $group entries-read] 5
        assert_equal [dict get $group lag] 1

        # When all the entries were deleted, the lag is always 0.
        r XTRIM x MAXLEN 0
        set reply [r XINFO STREAM x FULL]
        set group [lindex [dict get $reply groups] 0]
        assert_equal [dict get $group lag] 0
    }

    test {Loading from legacy (Redis <= v6.2.x, rdb_ver < 10) persistence} {
        # The payload was DUMPed from a v5 instance after:
        # XADD x 1-0 data a
        # XADD x 2-0 data b
        # XADD x 3-0 data c
        # XADD x 4-0 data d
        # XADD x 5-0 data e
        # XADD x 6-0 data f
        # XDEL x 3-0
        # XGROUP CREATE x g1 0
        # XGROUP CREATE x g2 0
        # XREADGROUP GROUP g1 c11 COUNT 4 STREAMS x >
        # XTRIM x MAXLEN = 2

        r DEL x
        r RESTORE x 0 "\x0F\x01\x10\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\xC3\x40\x4A\x40\x57\x16\x57\x00\x00\x00\x23\x00\x02\x01\x04\x01\x01\x01\x84\x64\x61\x74\x61\x05\x00\x01\x03\x01\x00\x20\x01\x03\x81\x61\x02\x04\x20\x0A\x00\x01\x40\x0A\x00\x62\x60\x0A\x00\x02\x40\x0A\x00\x63\x60\x0A\x40\x22\x01\x81\x64\x20\x0A\x40\x39\x20\x0A\x00\x65\x60\x0A\x00\x05\x40\x0A\x00\x66\x20\x0A\x00\xFF\x02\x06\x00\x02\x02\x67\x31\x05\x00\x04\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x3E\xF7\x83\x43\x7A\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x00\x3E\xF7\x83\x43\x7A\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x04\x00\x00\x00\x00\x00\x00\x00\x00\x3E\xF7\x83\x43\x7A\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x05\x00\x00\x00\x00\x00\x00\x00\x00\x3E\xF7\x83\x43\x7A\x01\x00\x00\x01\x01\x03\x63\x31\x31\x3E\xF7\x83\x43\x7A\x01\x00\x00\x04\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x04\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x05\x00\x00\x00\x00\x00\x00\x00\x00\x02\x67\x32\x00\x00\x00\x00\x09\x00\x3D\x52\xEF\x68\x67\x52\x1D\xFA"

        set reply [r XINFO STREAM x FULL]
        assert_equal [dict get $reply max-deleted-entry-id] "0-0"
        assert_equal [dict get $reply entries-added] 2
        set group [lindex [dict get $reply groups] 0]
        assert_equal [dict get $group entries-read] 1
        assert_equal [dict get $group lag] 1
        set group [lindex [dict get $reply groups] 1]
        assert_equal [dict get $group entries-read] 0
        assert_equal [dict get $group lag] 2
    }

    test {Loading from legacy (Redis <= v7.0.x, rdb_ver < 11) persistence} {
        # The payload was DUMPed from a v7 instance after:
        # XGROUP CREATE x g $ MKSTREAM
        # XADD x 1-1 f v
        # XREADGROUP GROUP g Alice STREAMS x >

        r DEL x
        r RESTORE x 0 "\x13\x01\x10\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x01\x1D\x1D\x00\x00\x00\x0A\x00\x01\x01\x00\x01\x01\x01\x81\x66\x02\x00\x01\x02\x01\x00\x01\x00\x01\x81\x76\x02\x04\x01\xFF\x01\x01\x01\x01\x01\x00\x00\x01\x01\x01\x67\x01\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x01\xF5\x5A\x71\xC7\x84\x01\x00\x00\x01\x01\x05\x41\x6C\x69\x63\x65\xF5\x5A\x71\xC7\x84\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x01\x0B\x00\xA7\xA9\x14\xA5\x27\xFF\x9B\x9B"
        set reply [r XINFO STREAM x FULL]
        set group [lindex [dict get $reply groups] 0]
        set consumer [lindex [dict get $group consumers] 0]
        assert_equal [dict get $consumer seen-time] [dict get $consumer active-time]
    }

    start_server {tags {"external:skip"}} {
        set master [srv -1 client]
        set master_host [srv -1 host]
        set master_port [srv -1 port]
        set slave [srv 0 client]

        foreach noack {0 1} {
            test "Consumer group last ID propagation to slave (NOACK=$noack)" {
                $slave slaveof $master_host $master_port
                wait_for_condition 50 100 {
                    [s 0 master_link_status] eq {up}
                } else {
                    fail "Replication not started."
                }

                $master del stream
                $master xadd stream * a 1
                $master xadd stream * a 2
                $master xadd stream * a 3
                $master xgroup create stream mygroup 0

                # Consume the first two items on the master
                for {set j 0} {$j < 2} {incr j} {
                    if {$noack} {
                        set item [$master xreadgroup group mygroup \
                                  myconsumer COUNT 1 NOACK STREAMS stream >]
                    } else {
                        set item [$master xreadgroup group mygroup \
                                  myconsumer COUNT 1 STREAMS stream >]
                    }
                    set id [lindex $item 0 1 0 0]
                    if {$noack == 0} {
                        assert {[$master xack stream mygroup $id] eq "1"}
                    }
                }

                wait_for_ofs_sync $master $slave

                # Turn slave into master
                $slave slaveof no one

                set item [$slave xreadgroup group mygroup myconsumer \
                          COUNT 1 STREAMS stream >]

                # The consumed entry should be the third
                set myentry [lindex $item 0 1 0 1]
                assert {$myentry eq {a 3}}
            }
        }
    }

    start_server {tags {"external:skip"}} {
        set master [srv -1 client]
        set master_host [srv -1 host]
        set master_port [srv -1 port]
        set replica [srv 0 client]

        foreach autoclaim {0 1} {
            test "Replication tests of XCLAIM with deleted entries (autoclaim=$autoclaim)" {
                $replica replicaof $master_host $master_port
                wait_for_condition 50 100 {
                    [s 0 master_link_status] eq {up}
                } else {
                    fail "Replication not started."
                }

                $master DEL x
                $master XADD x 1-0 f v
                $master XADD x 2-0 f v
                $master XADD x 3-0 f v
                $master XADD x 4-0 f v
                $master XADD x 5-0 f v
                $master XGROUP CREATE x grp 0
                assert_equal [$master XREADGROUP GROUP grp Alice STREAMS x >] {{x {{1-0 {f v}} {2-0 {f v}} {3-0 {f v}} {4-0 {f v}} {5-0 {f v}}}}}
                wait_for_ofs_sync $master $replica
                assert_equal [llength [$replica XPENDING x grp - + 10 Alice]] 5
                $master XDEL x 2-0
                $master XDEL x 4-0
                if {$autoclaim} {
                    assert_equal [$master XAUTOCLAIM x grp Bob 0 0-0] {0-0 {{1-0 {f v}} {3-0 {f v}} {5-0 {f v}}} {2-0 4-0}}
                    wait_for_ofs_sync $master $replica
                    assert_equal [llength [$replica XPENDING x grp - + 10 Alice]] 0
                } else {
                    assert_equal [$master XCLAIM x grp Bob 0 1-0 2-0 3-0 4-0] {{1-0 {f v}} {3-0 {f v}}}
                    wait_for_ofs_sync $master $replica
                    assert_equal [llength [$replica XPENDING x grp - + 10 Alice]] 1
                }
            }
        }

        test {XREADGROUP ACK would propagate entries-read} {
            $master del mystream
            $master xadd mystream * a b c d e f
            $master xgroup create mystream mygroup $
            $master xreadgroup group mygroup ryan count 1 streams mystream >
            $master xadd mystream * a1 b1 a1 b2
            $master xadd mystream * name v1 name v1
            $master xreadgroup group mygroup ryan count 1 streams mystream >
            $master xreadgroup group mygroup ryan count 1 streams mystream >

            set reply [$master XINFO STREAM mystream FULL]
            set group [lindex [dict get $reply groups] 0]
            assert_equal [dict get $group entries-read] 3
            assert_equal [dict get $group lag] 0

            wait_for_ofs_sync $master $replica

            set reply [$replica XINFO STREAM mystream FULL]
            set group [lindex [dict get $reply groups] 0]
            assert_equal [dict get $group entries-read] 3
            assert_equal [dict get $group lag] 0
        }

        test {XREADGROUP from PEL inside MULTI} {
            # This scenario used to cause propagation of EXEC without MULTI in 6.2
            $replica config set propagation-error-behavior panic
            $master del mystream
            $master xadd mystream 1-0 a b c d e f
            $master xgroup create mystream mygroup 0
            assert_equal [$master xreadgroup group mygroup ryan count 1 streams mystream >] {{mystream {{1-0 {a b c d e f}}}}}
            $master multi
            $master xreadgroup group mygroup ryan count 1 streams mystream 0
            $master exec
        }
    }

    start_server {tags {"stream needs:debug"} overrides {appendonly yes aof-use-rdb-preamble no}} {
        test {Empty stream with no lastid can be rewrite into AOF correctly} {
            r XGROUP CREATE mystream group-name $ MKSTREAM
            assert {[dict get [r xinfo stream mystream] length] == 0}
            set grpinfo [r xinfo groups mystream]
            r bgrewriteaof
            waitForBgrewriteaof r
            r debug loadaof
            assert {[dict get [r xinfo stream mystream] length] == 0}
            assert_equal [r xinfo groups mystream] $grpinfo
        }
    }

    start_server {} {
        test "XACKDEL wrong number of args" {
            assert_error {*wrong number of arguments for 'xackdel' command} {r XACKDEL}
            assert_error {*wrong number of arguments for 'xackdel' command} {r XACKDEL s}
            assert_error {*wrong number of arguments for 'xackdel' command} {r XACKDEL s g}
        }

        test "XACKDEL should return empty array when key doesn't exist or group doesn't exist" {
            r DEL s
            assert_equal {-1 -1} [r XACKDEL s g IDS 2 1-1 2-2] ;# the key doesn't exist

            r XADD s 1-0 f v
            assert_equal {-1 -1} [r XACKDEL s g IDS 2 1-1 2-2] ;# the key exists but the group doesn't exist
        }

        test "XACKDEL IDS parameter validation" {
            r DEL s
            r XADD s 1-0 f v
            r XGROUP CREATE s g 0

            # Test invalid numids
            assert_error {*Number of IDs must be a positive integer*} {r XACKDEL s g IDS abc 1-1}
            assert_error {*Number of IDs must be a positive integer*} {r XACKDEL s g IDS 0 1-1}
            assert_error {*Number of IDs must be a positive integer*} {r XACKDEL s g IDS -5 1-1}

            # Test whether numids is equal to the number of IDs provided
            assert_error {*The `numids` parameter must match the number of arguments*} {r XACKDEL s g IDS 3 1-1 2-2}
            assert_error {*syntax error*} {r XACKDEL s g IDS 1 1-1 2-2}
        }

        test "XACKDEL KEEPREF/DELREF/ACKED parameter validation" {
            # Test mutually exclusive options
            assert_error {*syntax error*} {r XACKDEL s g KEEPREF DELREF IDS 1 1-1}
            assert_error {*syntax error*} {r XACKDEL s g KEEPREF ACKED IDS 1 1-1}
            assert_error {*syntax error*} {r XACKDEL s g DELREF ACKED IDS 1 1-1}
        }

        test "XACKDEL with DELREF option acknowledges will remove entry from all PELs" {
            r DEL mystream
            r XADD mystream 1-0 f v
            r XADD mystream 2-0 f v

            # Create two consumer groups
            r XGROUP CREATE mystream group1 0
            r XGROUP CREATE mystream group2 0
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >
            r XREADGROUP GROUP group2 consumer2 STREAMS mystream >

            # Verify the message was removed from both groups' PELs when with DELREF
            assert_equal {1 1} [r XACKDEL mystream group1 DELREF IDS 2 1-0 2-0]
            assert_equal 0 [r XLEN mystream] 
            assert_equal {0 {} {} {}} [r XPENDING mystream group1]
            assert_equal {0 {} {} {}} [r XPENDING mystream group2] 
            assert_equal {-1 -1} [r XACKDEL mystream group2 DELREF IDS 2 1-0 2-0]
        }

        test "XACKDEL with ACKED option only deletes messages acknowledged by all groups" {
            r DEL mystream
            r XADD mystream 1-0 f v
            r XADD mystream 2-0 f v

            # Create two consumer groups
            r XGROUP CREATE mystream group1 0
            r XGROUP CREATE mystream group2 0
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >
            r XREADGROUP GROUP group2 consumer2 STREAMS mystream >

            # The message is referenced by two groups.
            # Even after one of them is ack, it still can't be deleted.
            assert_equal {2 2} [r XACKDEL mystream group1 ACKED IDS 2 1-0 2-0]
            assert_equal 2 [r XLEN mystream]
            assert_equal {0 {} {} {}} [r XPENDING mystream group1]
            assert_equal {2 1-0 2-0 {{consumer2 2}}} [r XPENDING mystream group2]

            # When these messages are dereferenced by all groups, they can be deleted.
            assert_equal {1 1} [r XACKDEL mystream group2 ACKED IDS 2 1-0 2-0]
            assert_equal 0 [r XLEN mystream]
            assert_equal {0 {} {} {}} [r XPENDING mystream group1]
            assert_equal {0 {} {} {}} [r XPENDING mystream group2]
        }

        test "XACKDEL with KEEPREF" {
            r DEL mystream
            r XADD mystream 1-0 f v
            r XADD mystream 2-0 f v

            # Create two consumer groups
            r XGROUP CREATE mystream group1 0
            r XGROUP CREATE mystream group2 0
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >
            r XREADGROUP GROUP group2 consumer2 STREAMS mystream >

            # Test XACKDEL with KEEPREF
            # XACKDEL only deletes the message from the stream
            # but does not clean up references in consumer groups' PELs
            assert_equal {1 1} [r XACKDEL mystream group1 KEEPREF IDS 2 1-0 2-0]
            assert_equal 0 [r XLEN mystream]
            assert_equal {0 {} {} {}} [r XPENDING mystream group1]
            assert_equal {2 1-0 2-0 {{consumer2 2}}} [r XPENDING mystream group2]

            # Acknowledge remaining messages in group2
            assert_equal {1 1} [r XACKDEL mystream group2 KEEPREF IDS 2 1-0 2-0]
            assert_equal {0 {} {} {}} [r XPENDING mystream group1]
            assert_equal {0 {} {} {}} [r XPENDING mystream group2]
        }

        test "XGROUP CREATE with ENTRIESREAD larger than stream entries should cap the value" {
            r DEL mystream
            r xadd mystream * field value
            r xgroup create mystream mygroup $ entriesread 9999

            set reply [r XINFO STREAM mystream FULL]
            set group [lindex [dict get $reply groups] 0]

            # Lag must be 0 and entries-read must be 1.
            assert_equal [dict get $group lag] 0
            assert_equal [dict get $group entries-read] 1
        }

        test "XGROUP SETID with ENTRIESREAD larger than stream entries should cap the value" {
            r DEL mystream
            r xadd mystream * field value
            r xgroup create mystream mygroup $

            r xgroup setid mystream mygroup $ entriesread 9999

            set reply [r XINFO STREAM mystream FULL]
            set group [lindex [dict get $reply groups] 0]

            # Lag must be 0 and entries-read must be 1.
            assert_equal [dict get $group lag] 0
            assert_equal [dict get $group entries-read] 1
        }

        test "XACKDEL with IDs exceeding STREAMID_STATIC_VECTOR_LEN for heap allocation" {
            r DEL mystream
            r XGROUP CREATE mystream mygroup $ MKSTREAM

            # Generate IDs exceeding STREAMID_STATIC_VECTOR_LEN (8) to force heap allocation
            # instead of using the static vector cache, ensuring proper memory allocation.
            set ids {}
            for {set i 0} {$i < 50} {incr i} {
                lappend ids "$i-1"
            }
            set result [r XACKDEL mystream mygroup IDS 50 {*}$ids]
            assert {[llength $result] == 50}
            r PING
        }
    }

    start_server {tags {"repl external:skip"}} {
        test "XREADGROUP CLAIM delivery count increments replicated correctly" {
            start_server {tags {"stream"}} {
                set master [srv 0 client]
                set master_host [srv 0 host]
                set master_port [srv 0 port]
                
                start_server {tags {"stream"}} {
                    set replica [srv 0 client]
                    
                    # Setup replication
                    $replica replicaof $master_host $master_port
                    wait_for_sync $replica
                    
                    # Setup stream and consumer group on master
                    $master DEL mystream
                    $master XADD mystream 1-0 f v1
                    $master XGROUP CREATE mystream group1 0
                    
                    # Wait for replication
                    wait_for_ofs_sync $master $replica
                    
                    # First read on master
                    $master XREADGROUP GROUP group1 consumer1 STREAMS mystream >
                    wait_for_ofs_sync $master $replica
                    
                    # Check initial delivery count on replica
                    set replica_pending [$replica XPENDING mystream group1 - + 1]
                    assert_equal [llength $replica_pending] 1
                    set delivery_count [lindex [lindex $replica_pending 0] 3]
                    assert_equal $delivery_count 1
                    
                    # First claim on master
                    after 100
                    set claim_result1 [$master XREADGROUP GROUP group1 consumer2 CLAIM 50 STREAMS mystream >]
                    wait_for_ofs_sync $master $replica
                    
                    # Check delivery count after first claim
                    set replica_pending [$replica XPENDING mystream group1 - + 1]
                    set delivery_count [lindex [lindex $replica_pending 0] 3]
                    assert_equal $delivery_count 2
                    
                    # Second claim on master
                    after 100
                    set claim_result2 [$master XREADGROUP GROUP group1 consumer3 CLAIM 50 STREAMS mystream >]
                    wait_for_ofs_sync $master $replica
                    
                    # Check final delivery count on replica
                    set replica_pending [$replica XPENDING mystream group1 - + 1]
                    assert_equal [llength $replica_pending] 1
                    set delivery_count [lindex [lindex $replica_pending 0] 3]
                    assert_equal $delivery_count 3
                }
            }
        }
    }

    start_server {tags {"repl external:skip" "stream"}} {
        # Verify that XREADGROUP propagates a newly created consumer to
        # the replica in cases where no XCLAIM is generated (XCLAIM
        # implicitly creates the consumer, so explicit propagation is
        # only needed when it is absent).  Two cases are tested:
        #   1. Without NOACK and no messages to deliver — no XCLAIM at all.
        #   2. With NOACK and messages delivered — NOACK skips PEL/XCLAIM.
        test "XREADGROUP propagates new consumer to replica" {
            set master [srv 0 client]
            set master_host [srv 0 host]
            set master_port [srv 0 port]

            start_server {tags {"stream"}} {
                set replica [srv 0 client]

                $replica replicaof $master_host $master_port
                wait_for_sync $replica

                $master DEL mystream
                $master XADD mystream 1-0 f v
                $master XGROUP CREATE mystream grp 0

                # Consume the only message so the stream has no
                # new messages pending for delivery.
                $master XREADGROUP GROUP grp c1 STREAMS mystream >
                $master XACK mystream grp 1-0

                wait_for_ofs_sync $master $replica

                # Case 1: XREADGROUP without NOACK for a brand-new
                # consumer when there are NO messages to deliver.
                # No XCLAIM is generated, so the consumer must be
                # explicitly propagated.
                set reply [$master XREADGROUP GROUP grp c2 STREAMS mystream >]
                assert_equal $reply {}

                set master_consumers [$master XINFO CONSUMERS mystream grp]
                set master_names [lmap c $master_consumers {dict get $c name}]
                assert {[lsearch $master_names "c2"] >= 0}

                wait_for_ofs_sync $master $replica

                set replica_consumers [$replica XINFO CONSUMERS mystream grp]
                set replica_names [lmap c $replica_consumers {dict get $c name}]
                if {[lsearch $replica_names "c2"] < 0} {
                    fail "Consumer 'c2' not found on replica (have: $replica_names)"
                }

                # Case 2: XREADGROUP with NOACK for a brand-new consumer
                # when a message IS available.  NOACK skips PEL/XCLAIM
                # entirely, so the consumer must be explicitly propagated
                # even though messages were delivered.
                $master XADD mystream 2-0 f v
                wait_for_ofs_sync $master $replica

                set reply [$master XREADGROUP GROUP grp c3 NOACK STREAMS mystream >]
                assert {$reply ne {}}

                set master_consumers [$master XINFO CONSUMERS mystream grp]
                set master_names [lmap c $master_consumers {dict get $c name}]
                assert {[lsearch $master_names "c3"] >= 0}

                wait_for_ofs_sync $master $replica

                set replica_consumers [$replica XINFO CONSUMERS mystream grp]
                set replica_names [lmap c $replica_consumers {dict get $c name}]
                if {[lsearch $replica_names "c3"] < 0} {
                    fail "Consumer 'c3' not found on replica (have: $replica_names)"
                }
            }
        }
    }

    start_server {} {
        if {!$::force_resp3} {
        test "XREADGROUP CLAIM field types are correct" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XGROUP CREATE mystream group1 0

            # Read the message with XREADGROUP
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >

            # Wait to allow claiming
            after 100

            # Read again with CLAIM using readraw to check field types
            r readraw 1
            r deferred 1
            
            r XREADGROUP GROUP group1 consumer2 CLAIM 50 STREAMS mystream >

            # Check the response format line by line
            # Response structure: *1 (outer array) -> *2 (stream name + messages array)
            assert_equal [r read] {*1}       ;# Outer array (1 stream)
            assert_equal [r read] {*2}       ;# Stream data (2 elements: stream name + messages)
            assert_equal [r read] {$8}       ;# Stream name length
            assert_equal [r read] {mystream} ;# Stream name
            assert_equal [r read] {*1}       ;# Messages array (1 message)
            assert_equal [r read] {*4}       ;# Message with 4 fields
            assert_equal [r read] {$3}       ;# Field 1: Message ID length
            assert_equal [r read] {1-0}      ;# Field 1: Message ID value
            assert_equal [r read] {*2}       ;# Field 2: Field-value pairs array
            assert_equal [r read] {$1}       ;# Field-value pair: key length
            assert_equal [r read] {f}        ;# Field-value pair: key
            assert_equal [r read] {$2}       ;# Field-value pair: value length
            assert_equal [r read] {v1}       ;# Field-value pair: value
            
            # Field 3: Delivery count - should be integer type (:)
            set delivery_count_type [r read]
            assert_match {:*} $delivery_count_type "Expected delivery count to be integer type (:), got: $delivery_count_type"
            
            # Field 4: Idle time - should be integer type (:)
            set idle_time_type [r read]
            assert_match {:*} $idle_time_type "Expected idle time to be integer type (:), got: $idle_time_type"
        }
        }

        # Restore connection state
        r readraw 0
        r deferred 0

        test "XREADGROUP CLAIM returns unacknowledged messages" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XADD mystream 2-0 f v2

            # Create consumer groups
            r XGROUP CREATE mystream group1 0

            # Verify we got 1 message without acknowledgment
            set read_result [r XREADGROUP GROUP group1 consumer1 STREAMS mystream >]
            assert_equal [llength [lindex [lindex $read_result 0] 1]] 2

            # Verify the messages are now in pending state
            set pending_info [r XPENDING mystream group1]
            assert_equal [lindex $pending_info 0] 2

            after 100

            # Claim pending messages
            set claim_result [r XREADGROUP GROUP group1 consumer2 CLAIM 50 STREAMS mystream >]

            lassign [lindex $claim_result 0] stream_name messages
            assert_equal $stream_name "mystream"
            assert_equal [llength $messages] 2

            # Check first message
            assert_equal [lindex $messages 0 0] 1-0
            assert_equal [lindex $messages 0 1] [list f v1]
            assert {[lindex $messages 0 2] >= 50}
            assert_equal [lindex $messages 0 3] 1

            # Check second message
            assert_equal [lindex $messages 1 0] 2-0
            assert_equal [lindex $messages 1 1] [list f v2]
            assert {[lindex $messages 1 2] >= 50}
            assert_equal [lindex $messages 1 3] 1

            # Verify pending list now shows messages belong to consumer2
            set pending_range [r XPENDING mystream group1 - + 10]
            assert_equal [llength $pending_range] 2
            
            # Check that messages are now assigned to consumer2
            assert_equal [lindex [lindex $pending_range 0] 1] "consumer2"
            assert_equal [lindex [lindex $pending_range 1] 1] "consumer2"
            
            # Verify delivery count was incremented in pending list
            assert_equal [lindex [lindex $pending_range 0] 3] 2
            assert_equal [lindex [lindex $pending_range 1] 3] 2
        }

        test "XREADGROUP CLAIM respects min-idle-time threshold" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XADD mystream 2-0 f v2

            # Create consumer groups
            r XGROUP CREATE mystream group1 0

            # Verify we got 1 message without acknowledgment
            set read_result [r XREADGROUP GROUP group1 consumer1 STREAMS mystream >]
            assert_equal [llength [lindex [lindex $read_result 0] 1]] 2

            # Verify the messages are now in pending state
            set pending_info [r XPENDING mystream group1]
            assert_equal [lindex $pending_info 0] 2

            # Claim pending messages
            set claim_result [r XREADGROUP GROUP group1 consumer2 CLAIM 100 STREAMS mystream >]

            assert_equal [llength $claim_result] 0
        }

        test "XREADGROUP CLAIM with COUNT limit" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XADD mystream 2-0 f v2
            r XADD mystream 3-0 f v3

            # Create consumer groups
            r XGROUP CREATE mystream group1 0

            # Verify we got 1 message without acknowledgment
            set read_result [r XREADGROUP GROUP group1 consumer1 STREAMS mystream >]
            assert_equal [llength [lindex [lindex $read_result 0] 1]] 3

            # Verify the messages are now in pending state
            set pending_info [r XPENDING mystream group1]
            assert_equal [lindex $pending_info 0] 3

            after 100

            # Claim pending messages
            set claim_result [r XREADGROUP GROUP group1 consumer2 COUNT 2 CLAIM 50 STREAMS mystream >]

            lassign [lindex $claim_result 0] stream_name messages
            assert_equal $stream_name "mystream"
            assert_equal [llength $messages] 2

            # Check first message
            assert_equal [lindex $messages 0 0] 1-0
            assert_equal [lindex $messages 0 1] [list f v1]
            assert {[lindex $messages 0 2] >= 50}
            assert_equal [lindex $messages 0 3] 1

            # Check second message
            assert_equal [lindex $messages 1 0] 2-0
            assert_equal [lindex $messages 1 1] [list f v2]
            assert {[lindex $messages 1 2] >= 50}
            assert_equal [lindex $messages 1 3] 1
        }

        test "XREADGROUP CLAIM without messages" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XDEL mystream 1-0

            # Create consumer groups
            r XGROUP CREATE mystream group1 0

            # Claim pending messages
            set claim_result [r XREADGROUP GROUP group1 consumer1 CLAIM 100 STREAMS mystream >]

            assert_equal [llength $claim_result] 0
        }

        test "XREADGROUP CLAIM without pending messages" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XADD mystream 2-0 f v2

            # Create consumer groups
            r XGROUP CREATE mystream group1 0

            # Claim pending messages
            set claim_result [r XREADGROUP GROUP group1 consumer1 CLAIM 100 STREAMS mystream >]

            lassign [lindex $claim_result 0] stream_name messages
            assert_equal $stream_name "mystream"
            assert_equal [llength $messages] 2

            # Check first message
            assert_equal [lindex $messages 0 0] 1-0
            assert_equal [lindex $messages 0 1] [list f v1]
            assert_equal [lindex $messages 0 2] 0
            assert_equal [lindex $messages 0 3] 0

            # Check second message
            assert_equal [lindex $messages 1 0] 2-0
            assert_equal [lindex $messages 1 1] [list f v2]
            assert_equal [lindex $messages 1 2] 0
            assert_equal [lindex $messages 1 3] 0
        }

        test "XREADGROUP CLAIM message response format" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XADD mystream 2-0 f v2

            # Create consumer groups
            r XGROUP CREATE mystream group1 0

            # Verify we got 1 message without acknowledgment
            set read_result [r XREADGROUP GROUP group1 consumer1 COUNT 1 STREAMS mystream >]
            assert_equal [llength [lindex [lindex $read_result 0] 1]] 1
            lassign [lindex $read_result 0] stream_name messages
            assert_equal $stream_name "mystream"
            assert_equal [llength [lindex $messages 0]] 2

            # Verify the messages are now in pending state
            set pending_info [r XPENDING mystream group1]
            assert_equal [lindex $pending_info 0] 1

            after 100

            # Claim pending messages
            set claim_result [r XREADGROUP GROUP group1 consumer2 CLAIM 50 STREAMS mystream >]

            lassign [lindex $claim_result 0] stream_name messages
            assert_equal $stream_name "mystream"

            # Check claimed message
            assert_equal [lindex $messages 0 0] 1-0
            assert_equal [lindex $messages 0 1] [list f v1]
            assert {[lindex $messages 0 2] >= 50 }
            assert_equal [lindex $messages 0 3] 1

            # Check stream message
            assert_equal [lindex $messages 1 0] 2-0
            assert_equal [lindex $messages 1 1] [list f v2]
            assert_equal [lindex $messages 1 2] 0
            assert_equal [lindex $messages 1 3] 0
        }

        test "XREADGROUP CLAIM delivery count" {
            r DEL mystream
            r XADD mystream 1-0 f v1

            # Create consumer groups
            r XGROUP CREATE mystream group1 0

            # Read message
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >

            after 100

            # Claim pending messages one time
            set claim_result [r XREADGROUP GROUP group1 consumer2 CLAIM 50 STREAMS mystream >]
            
            # Check delivery count
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal [lindex $messages 0 3] 1

            # Claim pending messages with same consumer three times
            after 100
            r XREADGROUP GROUP group1 consumer3 CLAIM 50 STREAMS mystream >

            after 100
            r XREADGROUP GROUP group1 consumer3 CLAIM 50 STREAMS mystream >

            after 100
            set claim_result [r XREADGROUP GROUP group1 consumer3 CLAIM 50 STREAMS mystream >]
            
            # Check delivery count
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal [lindex $messages 0 3] 4

            # Claim pending messages with different consumer two times
            after 100
            r XREADGROUP GROUP group1 consumer4 CLAIM 50 STREAMS mystream >

            after 100
            set claim_result [r XREADGROUP GROUP group1 consumer5 CLAIM 50 STREAMS mystream >]

            # Check delivery count
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal [lindex $messages 0 3] 6
        }

        test "XREADGROUP CLAIM idle time" {
            r DEL mystream
            r XADD mystream 1-0 f v1

            # Create consumer groups
            r XGROUP CREATE mystream group1 0

            # Read message
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >

            after 100
            # Claim pending messages
            set claim_result [r XREADGROUP GROUP group1 consumer2 CLAIM 50 STREAMS mystream >]
            # Check idle time
            lassign [lindex $claim_result 0] stream_name messages
            assert {[lindex $messages 0 2] >= 50}

            # Claim pending messages
            after 70
            set claim_result [r XREADGROUP GROUP group1 consumer3 CLAIM 60 STREAMS mystream >]
            # Check idle time
            lassign [lindex $claim_result 0] stream_name messages
            assert {[lindex $messages 0 2] >= 60}

            after 80
            # Claim pending messages
            set claim_result [r XREADGROUP GROUP group1 consumer3 CLAIM 70 STREAMS mystream >]
            # Check idle time
            lassign [lindex $claim_result 0] stream_name messages
            assert {[lindex $messages 0 2] >= 70}

            after 20
            # Claim pending messages
            set claim_result [r XREADGROUP GROUP group1 consumer3 CLAIM 10 STREAMS mystream >]
            # Check idle time
            lassign [lindex $claim_result 0] stream_name messages
            assert {[lindex $messages 0 2] >= 10}
        }

        test "XREADGROUP CLAIM with NOACK" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XADD mystream 2-0 f v2

            # Create consumer groups
            r XGROUP CREATE mystream group1 0

            after 100

            # Claim with NOACK
            set claim_result [r XREADGROUP GROUP group1 consumer1 NOACK CLAIM 50 STREAMS mystream >]
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal [llength $messages] 2

            # Verify there is no pending messages
            set pending_info [r XPENDING mystream group1]
            assert_equal [lindex $pending_info 0] 0

            # Claim again with NOACK
            set claim_result [r XREADGROUP GROUP group1 consumer1 NOACK CLAIM 50 STREAMS mystream >]
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal [llength $messages] 0
        }

        test "XREADGROUP CLAIM with NOACK and pending messages" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XADD mystream 2-0 f v2

            # Create consumer groups
            r XGROUP CREATE mystream group1 0

            r XREADGROUP GROUP group1 consumer1 COUNT 1 STREAMS mystream >

            # Verify there is one pending message
            set pending_info [r XPENDING mystream group1]
            assert_equal [lindex $pending_info 0] 1

            after 100

            # Claim with NOACK. We expect one pending message and one from the stream
            set claim_result [r XREADGROUP GROUP group1 consumer1 NOACK CLAIM 50 STREAMS mystream >]
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal [llength $messages] 2

            # Verify there is one pending messages
            set pending_info [r XPENDING mystream group1]
            assert_equal [lindex $pending_info 0] 1

            after 100

            # Claim again with NOACK. We expect only the pending message.
            set claim_result [r XREADGROUP GROUP group1 consumer1 NOACK CLAIM 50 STREAMS mystream >]
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal [llength $messages] 1
            assert_equal [lindex $messages 0 0] 1-0
        }

        test "XREADGROUP CLAIM with multiple streams" {
            r DEL mystream{t}1
            r XADD mystream{t}1 1-0 f v1
            r XADD mystream{t}1 2-0 f v2

            r DEL mystream{t}2
            r XADD mystream{t}2 3-0 f v1
            r XADD mystream{t}2 4-0 f v2

            r DEL mystream{t}3
            r XADD mystream{t}3 5-0 f v1
            r XADD mystream{t}3 6-0 f v2

            # Create consumer groups
            r XGROUP CREATE mystream{t}1 group1 0
            r XGROUP CREATE mystream{t}2 group1 0
            r XGROUP CREATE mystream{t}3 group1 0

            r XREADGROUP GROUP group1 consumer1 COUNT 1 STREAMS mystream{t}1 mystream{t}2 mystream{t}3 > > >

            after 100

            # Claim messages from multiply streams.
            set claim_result [r XREADGROUP GROUP group1 consumer1 CLAIM 50 STREAMS mystream{t}1 mystream{t}2 mystream{t}3 > > >]

            # We expect two messages from the first stream. One pending and one new.
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal $stream_name "mystream{t}1"
            assert_equal [llength $messages] 2
            # Pending message. 
            assert_equal [lindex $messages 0 0] 1-0
            assert_equal [lindex $messages 0 3] 1
            # New message
            assert_equal [lindex $messages 1 0] 2-0
            assert_equal [lindex $messages 1 3] 0

            # We expect two messages from the second stream. One pending and one new.
            lassign [lindex $claim_result 1] stream_name messages
            assert_equal $stream_name "mystream{t}2"
            assert_equal [llength $messages] 2
            # Pending message. 
            assert_equal [lindex $messages 0 0] 3-0
            assert_equal [lindex $messages 0 3] 1
            # New message
            assert_equal [lindex $messages 1 0] 4-0
            assert_equal [lindex $messages 1 3] 0

            # We expect two messages from the third stream. One pending and one new.
            lassign [lindex $claim_result 2] stream_name messages
            assert_equal $stream_name "mystream{t}3"
            assert_equal [llength $messages] 2
            # Pending message. 
            assert_equal [lindex $messages 0 0] 5-0
            assert_equal [lindex $messages 0 3] 1
            # New message
            assert_equal [lindex $messages 1 0] 6-0
            assert_equal [lindex $messages 1 3] 0
        }

        test "XREADGROUP CLAIM with min-idle-time equal to zero" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XADD mystream 2-0 f v2

            # Create consumer groups
            r XGROUP CREATE mystream group1 0

            # Read one message
            r XREADGROUP GROUP group1 consumer1 COUNT 1 STREAMS mystream >

            # Claim one message with min-idle-time=0
            set claim_result [r XREADGROUP GROUP group1 consumer1 CLAIM 0 STREAMS mystream >]

            # We expect two messages. One pending and one new.
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal $stream_name "mystream"
            assert_equal [llength $messages] 2
            # Pending message. 
            assert_equal [lindex $messages 0 0] 1-0
            assert_equal [lindex $messages 0 3] 1
            # New message
            assert_equal [lindex $messages 1 0] 2-0
            assert_equal [lindex $messages 1 3] 0
        }

        test "XREADGROUP CLAIM with large min-idle-time" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XADD mystream 2-0 f v2

            # Create consumer groups
            r XGROUP CREATE mystream group1 0

            # Read one message
            r XREADGROUP GROUP group1 consumer1 COUNT 1 STREAMS mystream >

            after 100

            # Claim one message with large min-idle-time
            set claim_result [r XREADGROUP GROUP group1 consumer1 CLAIM 9223372036854775807 STREAMS mystream >]

            # We expect only the new message.
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal $stream_name "mystream"
            assert_equal [llength $messages] 1
            # New message 
            assert_equal [lindex $messages 0 0] 2-0
            assert_equal [lindex $messages 0 3] 0
        }

        test "XREADGROUP CLAIM with not integer for min-idle-time" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XADD mystream 2-0 f v2

            # Create consumer groups
            r XGROUP CREATE mystream group1 0

            # Read one message
            r XREADGROUP GROUP group1 consumer1 COUNT 1 STREAMS mystream >

            assert_error "*ERR min-idle-time is not an integer*" {r XREADGROUP GROUP group1 consumer1 CLAIM test STREAMS mystream >}
            assert_error "*ERR min-idle-time is not an integer*" {r XREADGROUP GROUP group1 consumer1 CLAIM 5.5 STREAMS mystream >}
            assert_error "*ERR min-idle-time is not an integer*" {r XREADGROUP GROUP group1 consumer1 CLAIM 5,5 STREAMS mystream >}
            assert_error "*ERR min-idle-time is not an integer*" {r XREADGROUP GROUP group1 consumer1 CLAIM "10e" STREAMS mystream >}
            assert_error "*ERR min-idle-time is not an integer*" {r XREADGROUP GROUP group1 consumer1 CLAIM +10 STREAMS mystream >}
            assert_error "*ERR min-idle-time is not an integer*" {r XREADGROUP GROUP group1 consumer1 CLAIM *10 STREAMS mystream >}
            assert_error "*ERR min-idle-time is not an integer*" {r XREADGROUP GROUP group1 consumer1 CLAIM 10/2 STREAMS mystream >}
            assert_error "*ERR min-idle-time is not an integer*" {r XREADGROUP GROUP group1 consumer1 CLAIM 10*2 STREAMS mystream >}
            assert_error "*ERR min-idle-time is not an integer*" {r XREADGROUP GROUP group1 consumer1 CLAIM 10€ STREAMS mystream >}
        }

        test "XREADGROUP CLAIM with negative integer for min-idle-time" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XADD mystream 2-0 f v2

            # Create consumer groups
            r XGROUP CREATE mystream group1 0

            # Read one message
            r XREADGROUP GROUP group1 consumer1 COUNT 1 STREAMS mystream >

            assert_error "*ERR min-idle-time must be a positive integer*" {r XREADGROUP GROUP group1 consumer1 CLAIM -10 STREAMS mystream >}
            assert_error "*ERR min-idle-time must be a positive integer*" {r XREADGROUP GROUP group1 consumer1 CLAIM -42 STREAMS mystream >}
            assert_error "*ERR min-idle-time is not an integer*" {r XREADGROUP GROUP group1 consumer1 CLAIM -0 STREAMS mystream >}
            assert_error "*ERR min-idle-time is not an integer*" {r XREADGROUP GROUP group1 consumer1 CLAIM -5.5 STREAMS mystream >}
            assert_error "*ERR min-idle-time is not an integer*" {r XREADGROUP GROUP group1 consumer1 CLAIM -5,5 STREAMS mystream >}
            assert_error "*ERR min-idle-time is not an integer*" {r XREADGROUP GROUP group1 consumer1 CLAIM "-10e" STREAMS mystream >}
            assert_error "*ERR min-idle-time is not an integer*" {r XREADGROUP GROUP group1 consumer1 CLAIM -10/2 STREAMS mystream >}
            assert_error "*ERR min-idle-time is not an integer*" {r XREADGROUP GROUP group1 consumer1 CLAIM -10*2 STREAMS mystream >}
            assert_error "*ERR min-idle-time is not an integer*" {r XREADGROUP GROUP group1 consumer1 CLAIM (-10)*2 STREAMS mystream >}
        }

        test "XREADGROUP CLAIM with different position" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XADD mystream 2-0 f v2
            
            # Create consumer groups
            r XGROUP CREATE mystream group1 0
            
            # Read one message
            r XREADGROUP GROUP group1 consumer1 COUNT 1 STREAMS mystream >
            
            after 100
            
            # Claim one message with CLAIM option after COUNT
            set claim_result [r XREADGROUP GROUP group1 consumer1 COUNT 1 CLAIM 50 STREAMS mystream >]
            # We expect only the claimed message.
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal $stream_name "mystream"
            assert_equal [llength $messages] 1
            assert_equal [lindex $messages 0 0] 1-0
            assert_equal [lindex $messages 0 3] 1
            
            after 100
            
            # Claim one message with CLAIM option before COUNT
            set claim_result [r XREADGROUP GROUP group1 consumer1 CLAIM 50 COUNT 1 STREAMS mystream >]
            # We expect only the claimed message.
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal $stream_name "mystream"
            assert_equal [llength $messages] 1
            assert_equal [lindex $messages 0 0] 1-0
            assert_equal [lindex $messages 0 3] 2
            
            after 100
            
            # Claim one message with multiple CLAIM options
            set claim_result [r XREADGROUP GROUP group1 consumer1 CLAIM 50 COUNT 1 CLAIM 60 STREAMS mystream >]
            # We expect only the claimed message.
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal $stream_name "mystream"
            assert_equal [llength $messages] 1
            assert_equal [lindex $messages 0 0] 1-0
            assert_equal [lindex $messages 0 3] 3
            
            after 100

            # Claim one message with CLAIM option before GROUP
            set claim_result [r XREADGROUP CLAIM 50 GROUP group1 consumer1 COUNT 1 STREAMS mystream >]
            # We expect only the claimed message.
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal $stream_name "mystream"
            assert_equal [llength $messages] 1
            assert_equal [lindex $messages 0 0] 1-0
            assert_equal [lindex $messages 0 3] 4

            # Test error cases with invalid CLAIM syntax
            assert_error "*ERR min-idle-time is not an integer*" {r XREADGROUP GROUP group1 consumer1 CLAIM 10 CLAIM COUNT 1 STREAMS mystream >}
            assert_error "*ERR min-idle-time is not an integer*" {r XREADGROUP CLAIM GROUP group1 consumer1 COUNT 1 STREAMS mystream >}
            assert_error "*NOGROUP No such key*" {r XREADGROUP GROUP group1 consumer1 COUNT 1 STREAMS mystream CLAIM 50 >}
            assert_error "*ERR Unbalanced*" {r XREADGROUP GROUP group1 consumer1 COUNT 1 STREAMS mystream CLAIM >}
            assert_error "*ERR Invalid stream ID*" {r XREADGROUP GROUP group1 consumer1 COUNT 1 STREAMS mystream > CLAIM 50}
            assert_error "*ERR Unbalanced*" {r XREADGROUP GROUP group1 consumer1 COUNT 1 STREAMS mystream > CLAIM}
            assert_error "*ERR syntax error*" {r XREADGROUP GROUP group1 CLAIM 50 consumer1 STREAMS mystream >}
            assert_error "*ERR min-idle-time is not an integer*" {r XREADGROUP GROUP group1 consumer1 CLAIM STREAMS mystream >}
        } {} {external:skip}

        test "XREADGROUP CLAIM with specific ID" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XADD mystream 2-0 f v2
            r XADD mystream 3-0 f v3

            # Create consumer groups
            r XGROUP CREATE mystream group1 0

            # Read one message
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >

            after 100

            # Claim option is ignored when we specify ID different than >.
            set claim_result [r XREADGROUP GROUP group1 consumer1 CLAIM 1000 STREAMS mystream 0]

            # We expect only the new message.
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal $stream_name "mystream"
            assert_equal [llength $messages] 3
            assert_equal [llength [lindex $messages 0]] 2
        }

        test "XREADGROUP CLAIM on non-existing consumer group" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XADD mystream 2-0 f v2
            r XADD mystream 3-0 f v3

            # Create consumer groups
            r XGROUP CREATE mystream group1 0

            # Read all messages
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >

            after 100
            # We expect error. Group does not exists.
            assert_error "*NOGROUP No such key*" {r XREADGROUP GROUP not_existing_group consumer1 CLAIM 50 STREAMS mystream >}
        }

        test "XREADGROUP CLAIM on non-existing consumer" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XADD mystream 2-0 f v2
            r XADD mystream 3-0 f v3

            # Create consumer groups
            r XGROUP CREATE mystream group1 0

            # Read all messages
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >

            after 100
            # We expect 3 messages. Consumer is created if not exist.
            set claim_result [r XREADGROUP GROUP group1 consumer2 CLAIM 50 STREAMS mystream >]
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal $stream_name "mystream"
            assert_equal [llength $messages] 3
        }

        test "XREADGROUP CLAIM verify ownership transfer and delivery count updates" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XADD mystream 2-0 f v2
            r XADD mystream 3-0 f v3

            # Create consumer groups
            r XGROUP CREATE mystream group1 0

            # Read one message
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >

            after 100

            # Transfer ownership to consumer2
            set claim_result [r XREADGROUP GROUP group1 consumer2 CLAIM 50 STREAMS mystream >]
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal $stream_name "mystream"
            assert_equal [llength $messages] 3

            # Verify ownership transfer and delivery count updates
            set pending_info [r XPENDING mystream group1 - + 10]

            assert_equal [llength $pending_info] 3
            
            # Check first message entry
            assert_equal [lindex $pending_info 0 0] "1-0"
            assert_equal [lindex $pending_info 0 1] "consumer2"
            assert_equal [lindex $pending_info 0 3] 2
            
            # Check second message entry
            assert_equal [lindex $pending_info 1 0] "2-0"
            assert_equal [lindex $pending_info 1 1] "consumer2"
            assert_equal [lindex $pending_info 1 3] 2
            
            # Check third message entry
            assert_equal [lindex $pending_info 2 0] "3-0"
            assert_equal [lindex $pending_info 2 1] "consumer2"
            assert_equal [lindex $pending_info 2 3] 2
        }

        test "XREADGROUP CLAIM verify XACK removes messages from CLAIM pool" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XADD mystream 2-0 f v2
            r XADD mystream 3-0 f v3

            # Create consumer group
            r XGROUP CREATE mystream group1 0

            # Read all three messages with consumer1
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >

            # Acknowledge messages 1-0 and 3-0, leaving 2-0 pending
            r XACK mystream group1 1-0 3-0

            after 100

            # Claim pending messages older than 50ms for consumer2
            set claim_result [r XREADGROUP GROUP group1 consumer2 CLAIM 50 STREAMS mystream >]
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal $stream_name "mystream"

            # Should claim only message 2-0 (the unacknowledged one)
            assert_equal [llength $messages] 1
            assert_equal [lindex $messages 0 0] 2-0

            # Acknowledge message 2-0
            r XACK mystream group1 2-0

            after 100

            # Attempt to claim again - should return nothing since all messages are acknowledged
            set claim_result [r XREADGROUP GROUP group1 consumer2 CLAIM 50 STREAMS mystream >]
            assert_equal [llength $claim_result] 0
        }

        test "XREADGROUP CLAIM verify that XCLAIM updates delivery count" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XADD mystream 2-0 f v2
            r XADD mystream 3-0 f v3

            # Create consumer group
            r XGROUP CREATE mystream group1 0

            # Read all three messages with consumer1 (delivery count becomes 1 for all)
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >

            after 100

            # This increments delivery count to 2 for these messages
            r XCLAIM mystream group1 consumer3 50 2-0 3-0

            after 100

            # This should claim all three messages and increment their delivery counts
            set claim_result [r XREADGROUP GROUP group1 consumer2 CLAIM 50 STREAMS mystream >]
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal $stream_name "mystream"
            assert_equal [llength $messages] 3

            # Message 1-0: only claimed once via XREADGROUP (delivery count = 1)
            assert_equal [lindex $messages 0 0] 1-0
            assert_equal [lindex $messages 0 3] 1

            # Message 2-0: claimed via XCLAIM then XREADGROUP (delivery count = 2)
            assert_equal [lindex $messages 1 0] 2-0
            assert_equal [lindex $messages 1 3] 2

            # Message 3-0: claimed via XCLAIM then XREADGROUP (delivery count = 2)
            assert_equal [lindex $messages 2 0] 3-0
            assert_equal [lindex $messages 2 3] 2
        }

        test "XREADGROUP CLAIM verify forced entries are claimable" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XADD mystream 2-0 f v2
            r XADD mystream 3-0 f v3

            # Create consumer group
            r XGROUP CREATE mystream group1 0

            r XCLAIM mystream group1 consumer3 0 1-0 2-0 FORCE JUSTID

            # This should claim all three messages and increment their delivery counts
            set claim_result [r XREADGROUP GROUP group1 consumer2 CLAIM 0 COUNT 2 STREAMS mystream >]
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal $stream_name "mystream"
            assert_equal [llength $messages] 2

            # Message 1-0: only claimed once via XREADGROUP (delivery count = 1)
            assert_equal [lindex $messages 0 0] 1-0
            assert_equal [lindex $messages 0 3] 1

            # Message 2-0: claimed via XCLAIM then XREADGROUP (delivery count = 2)
            assert_equal [lindex $messages 1 0] 2-0
            assert_equal [lindex $messages 1 3] 1
        }

        test "XREADGROUP CLAIM with BLOCK zero" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XADD mystream 2-0 f v2
            r XADD mystream 3-0 f v3

            # Create consumer group
            r XGROUP CREATE mystream group1 0

            # Read all three messages with consumer1
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >

            set claim_result [r XREADGROUP GROUP group1 consumer1 BLOCK 1 STREAMS mystream >]
            assert_equal [llength $claim_result] 0

            set claim_result [r XREADGROUP GROUP group1 consumer1 BLOCK 100 CLAIM 500 STREAMS mystream >]
            assert_equal [llength $claim_result] 0

            after 100

            set claim_result [r XREADGROUP GROUP group1 consumer1 BLOCK 10000 CLAIM 50 STREAMS mystream >]
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal $stream_name "mystream"
            assert_equal [llength $messages] 3

            after 100

            set claim_result [r XREADGROUP GROUP group1 consumer1 BLOCK 0 CLAIM 50 STREAMS mystream >]
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal $stream_name "mystream"
            assert_equal [llength $messages] 3
        }

        test "XREADGROUP CLAIM with two blocked clients" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XDEL mystream 1-0

            # Create consumer group
            r XGROUP CREATE mystream group1 0 MKSTREAM

            # Create two deferring clients for blocking reads
            set rd1 [redis_deferring_client]
            set rd2 [redis_deferring_client]
            
            # Both clients issue blocking XREADGROUP commands
            $rd1 XREADGROUP GROUP group1 consumer1 BLOCK 0 CLAIM 100 STREAMS mystream ">"
            $rd2 XREADGROUP GROUP group1 consumer2 BLOCK 0 CLAIM 100 STREAMS mystream ">"
            
            # Wait for both clients to be blocked
            wait_for_blocked_clients_count 2

            r XADD mystream 2-0 f v2

            set result1 [$rd1 read]
            assert_equal [llength $result1] 1

            set result2 [$rd2 read]
            assert_equal [llength $result2] 1

            # Clean up
            $rd1 close
            $rd2 close   
        }

        test "XREADGROUP CLAIM messages become claimable during block" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XGROUP CREATE mystream group1 0
            
            # Consumer1 reads but doesn't ack
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >
            
            # Consumer2 blocks with CLAIM - message not yet claimable
            set rd [redis_deferring_client]
            $rd XREADGROUP GROUP group1 consumer2 BLOCK 5000 CLAIM 1000 STREAMS mystream >
            
            wait_for_blocked_client
            
            # Wait for message to become claimable (>1000ms)
            after 1500
            
            # Should unblock and return the now-claimable message
            set result [$rd read]
            assert_equal [llength $result] 1
            
            $rd close
        }

        test "XREADGROUP CLAIM block times out with no claimable messages" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XGROUP CREATE mystream group1 0
            
            # Read and immediately try to claim (not idle enough)
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >
            
            set start [clock milliseconds]
            set result [r XREADGROUP GROUP group1 consumer2 BLOCK 100 CLAIM 500 STREAMS mystream >]
            set elapsed [expr {[clock milliseconds] - $start}]
            
            # Should timeout and return empty
            assert_equal [llength $result] 0
            assert_range $elapsed 100 300
        }

        test "XREADGROUP CLAIM block with multiple streams, mixed claimable" {
            r DEL stream{t}1 stream{t}2
            r XADD stream{t}1 1-0 f v1
            r XADD stream{t}2 2-0 f v2
            
            r XGROUP CREATE stream{t}1 group1 0
            r XGROUP CREATE stream{t}2 group1 0
            
            # Reads from both
            r XREADGROUP GROUP group1 consumer1 COUNT 1 STREAMS stream{t}1 stream{t}2 > >
            
            after 100
            
            # Blocks with CLAIM - should get all messages
            set result [r XREADGROUP GROUP group1 consumer2 BLOCK 1000 CLAIM 50 STREAMS stream{t}1 stream{t}2 > >]
            
            assert_equal [llength $result] 2
            # stream1 should have claimable message
            lassign [lindex $result 0] stream_name messages
            assert_equal $stream_name "stream{t}1"
            assert_equal [llength $messages] 1
            
            # stream2 should be empty (message not yet read)
            lassign [lindex $result 1] stream_name messages
            assert_equal $stream_name "stream{t}2"
            assert_equal [llength $messages] 1
        }

        test "XREADGROUP CLAIM claims all pending immediately" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XGROUP CREATE mystream group1 0
            
            # Consumer1 reads
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >
            
            # Consumer2 immediately tries to claim with min-idle-time=0
            set result [r XREADGROUP GROUP group1 consumer2 BLOCK 1000 CLAIM 0 STREAMS mystream >]
            
            # Should immediately return without blocking
            lassign [lindex $result 0] stream_name messages
            assert_equal [llength $messages] 1
        }

        test "XREADGROUP CLAIM with BLOCK and NOACK" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XGROUP CREATE mystream group1 0
            
            # Consumer1 reads without ack
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >
            
            after 100
            
            # Consumer2 tries to claim with NOACK
            set result [r XREADGROUP GROUP group1 consumer2 BLOCK 1000 CLAIM 50 NOACK STREAMS mystream >]
            
            lassign [lindex $result 0] stream_name messages
            assert_equal [llength $messages] 1
            
            # Verify message still pending
            set pending [r XPENDING mystream group1 - + 10]
            assert_equal [llength $pending] 1
        }

        test "XREADGROUP CLAIM BLOCK wakes on new message before min-idle-time reached" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XGROUP CREATE mystream group1 0
            
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >
            
            set rd [redis_deferring_client]
            $rd XREADGROUP GROUP group1 consumer2 BLOCK 5000 CLAIM 1000 STREAMS mystream >
            
            wait_for_blocked_client
            
            after 100  # Before min-idle-time
            r XADD mystream 2-0 f v2
            
            set result [$rd read]

            # Unblock with new message immediately, not wait for CLAIM threshold
            lassign [lindex $result 0] stream_name messages
            assert_equal [llength $messages] 1
            
            $rd close
        }

        test "XREADGROUP CLAIM verify claiming order" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XADD mystream 2-0 f v2
            r XADD mystream 3-0 f v3
            r XADD mystream 4-0 f v4
            r XADD mystream 5-0 f v5
            r XADD mystream 6-0 f v6

            # Create consumer group
            r XGROUP CREATE mystream group1 0

            # Read all messages with consumer1 to make them pending
            r XREADGROUP GROUP group1 consumer1 COUNT 10 STREAMS mystream >

            # Now use XCLAIM to explicitly set different delivery times for each message
            # We'll set the delivery time backwards in time by different amounts
            # to create known idle time differences without actually waiting
            set current_time [r TIME]
            set current_ms [expr {[lindex $current_time 0] * 1000 + [lindex $current_time 1] / 1000}]
            
            # Set delivery times: 1-0 is oldest (5000ms ago), 6-0 is newest (100ms ago)
            # Use larger values for robustness against timing variations
            r XCLAIM mystream group1 consumer1 0 1-0 TIME [expr {$current_ms - 50000}] JUSTID
            r XCLAIM mystream group1 consumer1 0 2-0 TIME [expr {$current_ms - 40000}] JUSTID
            r XCLAIM mystream group1 consumer1 0 3-0 TIME [expr {$current_ms - 30000}] JUSTID
            r XCLAIM mystream group1 consumer1 0 4-0 TIME [expr {$current_ms - 20000}] JUSTID
            r XCLAIM mystream group1 consumer1 0 5-0 TIME [expr {$current_ms - 2000}] JUSTID
            r XCLAIM mystream group1 consumer1 0 6-0 TIME [expr {$current_ms - 1000}] JUSTID

            # Now claim with threshold of 250ms - should get 1-0, 2-0, 3-0, 4-0 in that order
            # (idle times: 50000, 40000, 30000, 20000ms all >= 10000ms)
            set claim_result [r XREADGROUP GROUP group1 consumer2 CLAIM 10000 COUNT 10 STREAMS mystream >]
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal $stream_name "mystream"
            assert_equal [llength $messages] 4
            
            # Verify order: oldest first
            assert_equal [lindex $messages 0 0] 1-0
            assert_equal [lindex $messages 1 0] 2-0
            assert_equal [lindex $messages 2 0] 3-0
            assert_equal [lindex $messages 3 0] 4-0

            # Claim with threshold of 1500ms - should get remaining 5-0
            # (idle time: 200ms >= 1500ms, but 6-0 with 1000ms < 1500ms)
            set claim_result [r XREADGROUP GROUP group1 consumer2 CLAIM 1500 COUNT 10 STREAMS mystream >]
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal $stream_name "mystream"
            assert_equal [llength $messages] 1
            
            assert_equal [lindex $messages 0 0] 5-0

            # Claim with threshold of 500ms - should get last one (6-0)
            # (idle time: 100ms >= 500ms)
            set claim_result [r XREADGROUP GROUP group1 consumer2 CLAIM 500 COUNT 10 STREAMS mystream >]
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal $stream_name "mystream"
            assert_equal [llength $messages] 1
            
            assert_equal [lindex $messages 0 0] 6-0
        }

        test "XREADGROUP CLAIM after consumer deleted with pending messages" {
            r DEL mystream
            r XADD mystream 1-0 f v1

            # Create consumer group
            r XGROUP CREATE mystream group1 0
            
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >
            r XGROUP DELCONSUMER mystream group1 consumer1
            
            set pending [r XPENDING mystream group1 - + 10]
            assert_equal [llength $pending] 0

            after 100

            # Orphaned pending messages are deleted.
            set claim_result [r XREADGROUP GROUP group1 consumer2 CLAIM 50 STREAMS mystream >]
            assert_equal [llength $claim_result] 0
        }

        test "XREADGROUP CLAIM after XGROUP SETID moves past pending messages" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XADD mystream 2-0 f v2

            # Create consumer group
            r XGROUP CREATE mystream group1 0
            
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >
            r XGROUP SETID mystream group1 2-0
            
            after 100

            # Pending messages are still claimable
            set claim_result [r XREADGROUP GROUP group1 consumer2 CLAIM 50 STREAMS mystream >]
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal $stream_name "mystream"
            assert_equal [llength $messages] 2
        }

        test "XREADGROUP CLAIM after XGROUP SETID moves before pending messages" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XADD mystream 2-0 f v2

            # Create consumer group
            r XGROUP CREATE mystream group1 0
            
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >
            r XREADGROUP GROUP group1 consumer2 CLAIM 0 STREAMS mystream >
            r XGROUP SETID mystream group1 0
            
            after 100

            # Pending messages are still claimable
            set claim_result [r XREADGROUP GROUP group1 consumer2 CLAIM 50 STREAMS mystream >]
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal $stream_name "mystream"
            assert_equal [llength $messages] 4

            # Message 1-0: claimed by consumer2 (delivery count = 2)
            assert_equal [lindex $messages 0 0] 1-0
            assert_equal [lindex $messages 0 3] 2

            # Message 2-0: claimed by consumer2 (delivery count = 2)
            assert_equal [lindex $messages 1 0] 2-0
            assert_equal [lindex $messages 1 3] 2

            # Message 1-0: claimed by consumer2 (delivery count = 0)
            assert_equal [lindex $messages 2 0] 1-0
            assert_equal [lindex $messages 2 3] 0

            # Message 2-0: claimed by consumer2 (delivery count = 0)
            assert_equal [lindex $messages 3 0] 2-0
            assert_equal [lindex $messages 3 3] 0

            after 100

            # Verify that pending messages are not doubled
            set claim_result [r XREADGROUP GROUP group1 consumer2 CLAIM 50 STREAMS mystream >]
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal $stream_name "mystream"
            assert_equal [llength $messages] 2

            # Message 1-0: claimed by consumer2 (delivery count = 1)
            assert_equal [lindex $messages 0 0] 1-0
            assert_equal [lindex $messages 0 3] 1

            # Message 2-0: claimed by consumer2 (delivery count = 1)
            assert_equal [lindex $messages 1 0] 2-0
            assert_equal [lindex $messages 1 3] 1
        }

        test "XREADGROUP CLAIM when pending messages get trimmed" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XADD mystream 2-0 f v2
            r XADD mystream 3-0 f v3

            # Create consumer group
            r XGROUP CREATE mystream group1 0
            
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >
            
            # Trim away the pending messages
            r XTRIM mystream MAXLEN 0
            
            after 100

            # Pending list still references trimmed messages but they don't exist. We can't return them.
            set claim_result [r XREADGROUP GROUP group1 consumer2 CLAIM 50 STREAMS mystream >]
            assert_equal [llength $claim_result] 0
        }

        test "XREADGROUP CLAIM state persists across RDB save/load" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XADD mystream 2-0 f v2
            r XADD mystream 3-0 f v3
            
            r XGROUP CREATE mystream group1 0
            
            # Read messages to create pending entries
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >
            
            after 100
            
            # Claim some messages to increment delivery count
            r XREADGROUP GROUP group1 consumer2 CLAIM 50 STREAMS mystream >
            
            # Trigger RDB save and restart
            r SAVE
            r DEBUG RELOAD
            
            # Verify pending state restored
            set pending_info [r XPENDING mystream group1 - + 10]
            assert_equal [llength $pending_info] 3
            
            # Check first message entry
            assert_equal [lindex $pending_info 0 0] "1-0"
            assert_equal [lindex $pending_info 0 1] "consumer2"
            assert_equal [lindex $pending_info 0 3] 2

            # Check second message entry
            assert_equal [lindex $pending_info 1 0] "2-0"
            assert_equal [lindex $pending_info 1 1] "consumer2"
            assert_equal [lindex $pending_info 1 3] 2

            # Check third message entry
            assert_equal [lindex $pending_info 2 0] "3-0"
            assert_equal [lindex $pending_info 2 1] "consumer2"
            assert_equal [lindex $pending_info 2 3] 2
            
            # Verify can still claim after reload
            after 100
            set claim_result [r XREADGROUP GROUP group1 consumer3 CLAIM 50 STREAMS mystream >]
            lassign [lindex $claim_result 0] stream_name messages
            assert_equal $stream_name "mystream"
            assert_equal [llength $messages] 3

            # Message 1-0: claimed by consumer3 (delivery count = 2)
            assert_equal [lindex $messages 0 0] 1-0
            assert_equal [lindex $messages 0 3] 2

            # Message 2-0: claimed by consumer3 (delivery count = 2)
            assert_equal [lindex $messages 1 0] 2-0
            assert_equal [lindex $messages 1 3] 2

            # Message 2-0: claimed by consumer3 (delivery count = 2)
            assert_equal [lindex $messages 2 0] 3-0
            assert_equal [lindex $messages 2 3] 2
        } {} {external:skip needs:debug}

        test "XREADGROUP CLAIM idle time resets after RDB reload" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XGROUP CREATE mystream group1 0
            
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >
            
            after 1000
            
            # Before reload: message should be claimable
            set claim_before [r XREADGROUP GROUP group1 consumer2 CLAIM 500 STREAMS mystream >]
            assert_equal [llength [lindex $claim_before 0 1]] 1
            
            r SAVE
            r DEBUG RELOAD

            # After reload: idle time resets, message not immediately claimable
            set claim_after [r XREADGROUP GROUP group1 consumer3 CLAIM 500 STREAMS mystream >]
            assert_equal [llength $claim_after] 0

        } {} {external:skip needs:debug}

        test "XREADGROUP CLAIM multiple groups persist correctly" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XADD mystream 2-0 f v2
            
            r XGROUP CREATE mystream group1 0
            r XGROUP CREATE mystream group2 0
            
            r XREADGROUP GROUP group1 consumer1 COUNT 1 STREAMS mystream >
            r XREADGROUP GROUP group2 consumer1 STREAMS mystream >
            
            after 100
            r XREADGROUP GROUP group1 consumer2 CLAIM 50 STREAMS mystream >
            
            r SAVE
            r DEBUG RELOAD
            
            # Verify both groups maintained separately
            set pending1 [r XPENDING mystream group1]
            set pending2 [r XPENDING mystream group2]
            
            assert_equal [lindex $pending1 0] 2  ;# group1 has 2 pending
            assert_equal [lindex $pending2 0] 2  ;# group2 has 2 pending
        } {} {external:skip needs:debug}

        test "XREADGROUP CLAIM NOACK state not persisted" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XGROUP CREATE mystream group1 0
            
            after 100
            r XREADGROUP GROUP group1 consumer1 NOACK CLAIM 50 STREAMS mystream >
            
            set pending_before [r XPENDING mystream group1]
            assert_equal [lindex $pending_before 0] 0
            
            r SAVE
            r DEBUG RELOAD
            
            set pending_after [r XPENDING mystream group1]
            assert_equal [lindex $pending_after 0] 0
        } {} {external:skip needs:debug}

        test "XREADGROUP CLAIM high delivery counts persist in RDB" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XGROUP CREATE mystream group1 0
            
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >
            
            # Claim multiple times to increase delivery count
            for {set i 0} {$i < 10} {incr i} {
                after 20
                r XREADGROUP GROUP group1 consumer2 CLAIM 10 STREAMS mystream >
            }
            
            set pending_before [r XPENDING mystream group1 - + 1]
            set delivery_before [lindex $pending_before 0 3]
            
            r SAVE
            r DEBUG RELOAD

            set pending_after [r XPENDING mystream group1 - + 1]
            set delivery_after [lindex $pending_after 0 3]
            
            assert_equal $delivery_before $delivery_after
        } {} {external:skip needs:debug}

        test "XREADGROUP CLAIM usage stability with repeated claims" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XGROUP CREATE mystream group1 0
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >
            
            # Claim same message many times between consumers
            for {set i 0} {$i < 1000} {incr i} {
                after 2
                set consumer_id [expr {$i % 10 + 1}]
                r XREADGROUP GROUP group1 consumer$consumer_id CLAIM 1 STREAMS mystream >
            }
            
            # Verify no memory leaks - PEL should still have only 1 message
            set pending [r XPENDING mystream group1]
            assert_equal [lindex $pending 0] 1
        }

        test "XREADGROUP CLAIM with large number of PEL messages" {
            r DEL mystream
            r XGROUP CREATE mystream group1 0 MKSTREAM
            
            # Create large PEL
            for {set i 0} {$i < 10000} {incr i} {
                r XADD mystream * field $i
            }
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >
            
            after 100
            
            set result [r XREADGROUP GROUP group1 consumer2 CLAIM 50 COUNT 1000 STREAMS mystream >]
            assert_equal [llength [lindex $result 0 1]] 1000
        }

        test "XREADGROUP CLAIM within MULTI/EXEC transaction" {
            r DEL mystream
            r XADD mystream 1-0 f v1
            r XGROUP CREATE mystream group1 0
            r XREADGROUP GROUP group1 consumer1 STREAMS mystream >
            
            after 100
            
            r MULTI
            r XREADGROUP GROUP group1 consumer2 CLAIM 50 STREAMS mystream >
            r XPENDING mystream group1
            set results [r EXEC]
            
            # Verify transaction atomicity
            assert_equal [lindex $results 1 0] 1
        }

        test "XREAD with CLAIM option" {
            r DEL mystream
            r XADD mystream 1-0 f v1

            assert_error "*ERR The CLAIM option is only supported*" {r XREAD COUNT 2 CLAIM 10 STREAMS mystream 0-0}
        }
    }

    # Verify that XNACK rejects every invalid invocation with the correct error.
    # Covers: wrong argument count, nonexistent key/group (NOGROUP), wrong key
    # type (WRONGTYPE), unrecognized options at every position the parser
    # accepts them, invalid mode names, duplicate mode words, missing/bad IDS
    # keyword, bad numids (non-integer, zero, negative, mismatch), invalid
    # stream-ID format, RETRYCOUNT edge cases (non-integer, negative, overflow,
    # missing value, missing IDS), and extra trailing arguments.
    test "XNACK argument and error validation" {
        # Wrong number of arguments (no stream needed)
        assert_error "*wrong number of arguments*" {r XNACK}
        assert_error "*wrong number of arguments*" {r XNACK key}
        assert_error "*wrong number of arguments*" {r XNACK key group}
        assert_error "*wrong number of arguments*" {r XNACK key group SILENT}
        assert_error "*wrong number of arguments*" {r XNACK key group SILENT IDS}
        assert_error "*wrong number of arguments*" {r XNACK key group SILENT IDS 1}

        # Non-existent key / group
        r DEL nosuchkey
        assert_error "*NOGROUP*" {r XNACK nosuchkey grp SILENT IDS 1 1-0}

        r DEL mystream
        r XADD mystream 1-0 f v
        assert_error "*NOGROUP*" {r XNACK mystream nogroup SILENT IDS 1 1-0}

        # Wrong key type
        r DEL mykey
        r SET mykey "not a stream"
        assert_error "*WRONGTYPE*" {r XNACK mykey grp FAIL IDS 1 1-0}

        # All remaining checks need a stream + group + consumer
        r DEL mystream
        r XADD mystream 1-0 f v1
        r XADD mystream 2-0 f v2
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 STREAMS mystream >

        # Unrecognized option at various positions — the parser accepts options
        # both before and after the IDS block, so verify rejection in each slot.
        assert_error "ERR Unrecognized XNACK option*" {r XNACK mystream grp FAIL BADOPT IDS 1 1-0}
        assert_error "ERR Unrecognized XNACK option*" {r XNACK mystream grp FAIL IDS 1 1-0 BADOPT}
        assert_error "ERR Unrecognized XNACK option*" {r XNACK mystream grp SILENT BADOPT IDS 1 1-0 FORCE}
        assert_error "ERR Unrecognized XNACK option*" {r XNACK mystream grp SILENT FORCE BADOPT IDS 1 1-0}
        assert_error "ERR Unrecognized XNACK option*" {r XNACK mystream grp FAIL RETRYCOUNT 5 BADOPT IDS 1 1-0}
        assert_error "ERR Unrecognized XNACK option*" {r XNACK mystream grp FAIL IDS 1 1-0 RETRYCOUNT 5 BADOPT}
        assert_error "ERR Unrecognized XNACK option*" {r XNACK mystream grp FAIL FORCE IDS 1 1-0 BADOPT RETRYCOUNT 5}

        # Invalid mode
        assert_error "ERR mode must be SILENT, FAIL, or FATAL" {r XNACK mystream grp BADMODE IDS 1 1-0}

        # Multiple mode words — only one mode is allowed per invocation.
        assert_error "ERR Unrecognized XNACK option*" {r XNACK mystream grp FAIL FATAL IDS 1 1-0}
        assert_error "ERR Unrecognized XNACK option*" {r XNACK mystream grp SILENT FAIL IDS 1 1-0}
        assert_error "ERR Unrecognized XNACK option*" {r XNACK mystream grp FATAL SILENT IDS 1 1-0}
        assert_error "ERR Unrecognized XNACK option*" {r XNACK mystream grp FAIL SILENT FATAL IDS 1 1-0}

        # IDS keyword validation
        assert_error "ERR Unrecognized XNACK option*" {r XNACK mystream grp SILENT NOTIDS 1 1-0}
        assert_error "ERR syntax error, expected IDS keyword" {r XNACK mystream grp SILENT FORCE RETRYCOUNT 5}

        # numids validation
        assert_error "ERR numids must be a positive integer*" {r XNACK mystream grp SILENT IDS abc 1-0}
        assert_error "ERR numids must be a positive integer*" {r XNACK mystream grp SILENT IDS 0 1-0}
        assert_error "ERR numids must be a positive integer*" {r XNACK mystream grp SILENT IDS -1 1-0}
        assert_error "ERR number of IDs doesn't match numids" {r XNACK mystream grp SILENT IDS 2 1-0}

        # Invalid stream ID format
        assert_error "ERR Invalid stream ID*" {r XNACK mystream grp FAIL IDS 1 not-a-valid-id}

        # RETRYCOUNT validation — non-integer, negative, overflow, missing value
        assert_error "ERR value is not an integer or out of range" {r XNACK mystream grp FAIL IDS 1 1-0 RETRYCOUNT abc}
        assert_error "ERR Invalid RETRYCOUNT value, must be >= 0" {r XNACK mystream grp FAIL IDS 1 1-0 RETRYCOUNT -1}
        assert_error "ERR value is not an integer or out of range" {r XNACK mystream grp FAIL IDS 1 1-0 RETRYCOUNT 99999999999999999999}
        # RETRYCOUNT without a following value — consumed as trailing option
        assert_error "ERR Unrecognized XNACK option*" {r XNACK mystream grp FAIL IDS 1 1-0 RETRYCOUNT}
        # RETRYCOUNT right after mode with no IDS — too few arguments
        assert_error "ERR wrong number of arguments for 'xnack' command" {r XNACK mystream grp FAIL RETRYCOUNT}

        # Extra args after numids IDs — the surplus ID is parsed as an option
        assert_error "ERR Unrecognized XNACK option*" {r XNACK mystream grp FAIL IDS 1 1-0 2-0}
    }

    # Verify SILENT mode decrements delivery_count by 1, clamped at 0.
    # XPENDING format per entry: {id consumer idle delivery_count}.
    # After XNACK, consumer becomes {} (unowned) and idle becomes -1
    # (delivery_time reset to 0).
    test "XNACK SILENT mode delivery_count behavior" {
        r DEL mystream
        r XADD mystream 1-0 f v
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 STREAMS mystream >

        # delivery_count is 1 after XREADGROUP; SILENT decrements to 0
        set pending [r XPENDING mystream grp - + 10]
        assert_equal [lindex $pending 0 3] 1
        assert_equal 1 [r XNACK mystream grp SILENT IDS 1 1-0]
        set pending [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending] 1
        assert_equal [lindex $pending 0] {1-0 {} -1 0}

        # Clamp at 0: reclaim with RETRYCOUNT 0, then SILENT must not go below 0
        r XCLAIM mystream grp c2 0 1-0 RETRYCOUNT 0
        r XNACK mystream grp SILENT IDS 1 1-0
        set pending [r XPENDING mystream grp - + 10]
        assert_equal [lindex $pending 0 3] 0

        # Decrement from higher value: XCLAIM bumps delivery_count each time
        r XCLAIM mystream grp c1 0 1-0
        r XCLAIM mystream grp c1 0 1-0
        r XCLAIM mystream grp c1 0 1-0
        set pending [r XPENDING mystream grp - + 10]
        assert_equal [lindex $pending 0 3] 3
        assert_equal 1 [r XNACK mystream grp SILENT IDS 1 1-0]
        set pending [r XPENDING mystream grp - + 10]
        # 3 - 1 = 2
        assert_equal [lindex $pending 0] {1-0 {} -1 2}
    }

    # Verify FAIL mode NACKs the entry (makes it unowned) but preserves the
    # original delivery_count. The count stays at 1 (set by XREADGROUP).
    test "XNACK FAIL mode keeps delivery_count unchanged" {
        r DEL mystream
        r XADD mystream 1-0 f v
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 STREAMS mystream >

        set pending [r XPENDING mystream grp - + 10]
        assert_equal [lindex $pending 0 3] 1

        assert_equal 1 [r XNACK mystream grp FAIL IDS 1 1-0]

        set pending [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending] 1
        # delivery_count unchanged at 1
        assert_equal [lindex $pending 0] {1-0 {} -1 1}
    }

    # Verify FATAL mode sets delivery_count to LLONG_MAX (9223372036854775807),
    # signaling permanent/unrecoverable failure for this entry.
    test "XNACK FATAL mode sets delivery_count to max" {
        r DEL mystream
        r XADD mystream 1-0 f v
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 STREAMS mystream >

        assert_equal 1 [r XNACK mystream grp FATAL IDS 1 1-0]

        set pending [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending] 1
        # 9223372036854775807 == LLONG_MAX
        assert_equal [lindex $pending 0] {1-0 {} -1 9223372036854775807}
    }

    # Verify that XNACK removes entries from the consumer-level PEL (the entry
    # becomes unowned) while keeping them in the group-level PEL.
    # Setup: c1 owns {1-0, 2-0}, c2 owns {3-0}. NACK entries from both
    # consumers and confirm the ownership transfer.
    # Also verifies that XNACK does not auto-create or destroy consumers.
    test "XNACK releases entries and removes from consumer PEL" {
        r DEL mystream
        r XADD mystream 1-0 f v1
        r XADD mystream 2-0 f v2
        r XADD mystream 3-0 f v3
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 COUNT 2 STREAMS mystream >
        r XREADGROUP GROUP grp c2 COUNT 1 STREAMS mystream >

        # XNACK entries owned by different consumers
        assert_equal 1 [r XNACK mystream grp FAIL IDS 1 3-0]
        assert_equal 1 [r XNACK mystream grp FAIL IDS 1 1-0]

        # Both NACKed entries should be unowned in the group PEL
        set pending [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending] 3
        assert_equal [lindex $pending 0] {1-0 {} -1 1}
        assert_equal [lindex $pending 2] {3-0 {} -1 1}

        # Consumer-level PEL: c1 only has 2-0 left, c2 has nothing
        set c1_pending [r XPENDING mystream grp - + 10 c1]
        assert_equal [llength $c1_pending] 1
        assert_equal [lindex $c1_pending 0 0] 2-0
        set c2_pending [r XPENDING mystream grp - + 10 c2]
        assert_equal [llength $c2_pending] 0

        # XNACK does not auto-create or destroy consumers
        set info [r XINFO GROUPS mystream]
        assert_equal [dict get [lindex $info 0] consumers] 2
    }

    # Verify the integer return value of XNACK (number of entries successfully
    # NACKed) and several edge cases:
    #  - IDs not in the PEL are silently skipped (return 0).
    #  - Multiple IDs can be NACKed in a single call.
    #  - When valid and non-PEL IDs are mixed, only valid ones are counted.
    #  - Duplicate IDs: each occurrence is counted separately.
    #  - NACKing against an empty PEL returns 0.
    test "XNACK return count and edge cases" {
        r DEL mystream
        r XADD mystream 1-0 f v1
        r XADD mystream 2-0 f v2
        r XADD mystream 3-0 f v3
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 STREAMS mystream >

        # Skips IDs not in PEL
        assert_equal 0 [r XNACK mystream grp FAIL IDS 1 9-9]

        # Multiple IDs at once
        assert_equal 3 [r XNACK mystream grp SILENT IDS 3 1-0 2-0 3-0]
        set pending [r XPENDING mystream grp - + 10]
        assert_equal $pending {{1-0 {} -1 0} {2-0 {} -1 0} {3-0 {} -1 0}}

        # Reclaim all entries back to c1 for further sub-tests
        r XCLAIM mystream grp c1 0 1-0 2-0 3-0

        # Mixed valid and invalid IDs: only the 3 valid ones are counted
        assert_equal 3 [r XNACK mystream grp FAIL IDS 5 1-0 9-9 2-0 8-8 3-0]
        set pending [r XPENDING mystream grp - + 10]
        foreach entry $pending {
            assert_equal [lindex $entry 1] {}
        }

        # Duplicate IDs: the first NACK finds a consumer-owned entry, the
        # second finds an already-NACKed entry — both count as successful.
        r XCLAIM mystream grp c1 0 1-0
        assert_equal 2 [r XNACK mystream grp FAIL IDS 2 1-0 1-0]
        set pending [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending] 3
        assert_equal [lindex $pending 0] {1-0 {} -1 2}

        # Empty PEL returns 0
        r XACK mystream grp 1-0 2-0 3-0
        set info [r XPENDING mystream grp]
        assert_equal [lindex $info 0] 0
        assert_equal 0 [r XNACK mystream grp FAIL IDS 1 1-0]
    }

    # Verify behavior when re-NACKing an entry that is already in the NACK
    # zone (unowned). Each mode still applies its delivery_count semantics:
    #  - FAIL is idempotent (count unchanged, returns 1).
    #  - SILENT still decrements.
    #  - FATAL still sets to LLONG_MAX.
    # Mode transitions on already-NACKed entries work correctly.
    test "XNACK on already-NACKed entry: idempotency and mode changes" {
        r DEL mystream
        r XADD mystream 1-0 f v
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 STREAMS mystream >

        # Re-NACK with FAIL: returns 1, count unchanged
        assert_equal 1 [r XNACK mystream grp FAIL IDS 1 1-0]
        assert_equal 1 [r XNACK mystream grp FAIL IDS 1 1-0]
        set pending [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending] 1
        assert_equal [lindex $pending 0] {1-0 {} -1 1}

        # SILENT on already-NACKed: decrements 1 to 0
        assert_equal 1 [r XNACK mystream grp SILENT IDS 1 1-0]
        set pending [r XPENDING mystream grp - + 10]
        assert_equal [lindex $pending 0 3] 0

        # FATAL on already-NACKed: sets to LLONG_MAX
        assert_equal 1 [r XNACK mystream grp FATAL IDS 1 1-0]
        set pending [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending] 1
        assert_equal [lindex $pending 0] {1-0 {} -1 9223372036854775807}

        # FAIL on already-NACKed: returns success (idempotent)
        assert_equal 1 [r XNACK mystream grp FAIL IDS 1 1-0]
    }

    # Verify that NACKed entries form a "NACK zone" at the head of the
    # time-ordered PEL with FIFO insertion order.
    # NACKed entries have delivery_time=0, so XPENDING reports idle=-1.
    # XINFO STREAM FULL iterates the PEL rax by stream-ID order (not NACK
    # order), so we check both views to confirm correct state.
    test "XNACK ordering: NACKed entries at head of PEL with FIFO order" {
        r DEL mystream
        r XADD mystream 1-0 f v1
        r XADD mystream 2-0 f v2
        r XADD mystream 3-0 f v3
        r XADD mystream 4-0 f v4
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 STREAMS mystream >

        # XNACK in non-sequential stream-ID order: 3-0 first, then 1-0
        r XNACK mystream grp FAIL IDS 1 3-0
        r XNACK mystream grp FAIL IDS 1 1-0

        # NACKed entries should have delivery_time=0 (idle=-1 in XPENDING)
        set pending [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending] 4

        foreach entry $pending {
            set id [lindex $entry 0]
            if {$id eq "3-0" || $id eq "1-0"} {
                assert_equal [lindex $entry 1] {} ;# unowned
                assert_equal [lindex $entry 2] -1 ;# idle is -1 because delivery_time is 0
            } else {
                assert_equal [lindex $entry 1] c1 ;# still owned
            }
        }

        # XINFO STREAM FULL iterates the PEL rax by stream ID order.
        # NACKed entries show delivery_time=0 and consumer={}.
        set info [r XINFO STREAM mystream FULL]
        set group [lindex [dict get $info groups] 0]
        set pel [dict get $group pending]

        assert_equal [lindex $pel 0] {1-0 {} 0 1}
        assert_match {2-0 c1 * 1} [lindex $pel 1]
        assert_equal [lindex $pel 2] {3-0 {} 0 1}
        assert_match {4-0 c1 * 1} [lindex $pel 3]
    }

    # Verify that NACKed PEL entries survive deletion of the underlying stream
    # entry. Both XDEL (single entry removal) and XTRIM (bulk trimming) must
    # not remove PEL entries — they become "ghost" entries that are cleaned up
    # only when claimed (XCLAIM/XAUTOCLAIM) or acknowledged (XACK).
    test "XNACK NACKed entries persist after XDEL and XTRIM" {
        # XDEL case: delete the stream entry, PEL entry stays
        r DEL mystream
        r XADD mystream 1-0 f v1
        r XADD mystream 2-0 f v2
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 STREAMS mystream >
        r XNACK mystream grp FAIL IDS 1 1-0
        r XDEL mystream 1-0
        set pending [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending] 2
        assert_equal [lindex $pending 0] {1-0 {} -1 1}

        # XTRIM case: trim all but the last entry, PEL entries remain
        r DEL mystream
        r XADD mystream 1-0 f v1
        r XADD mystream 2-0 f v2
        r XADD mystream 3-0 f v3
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 STREAMS mystream >
        r XNACK mystream grp FAIL IDS 1 1-0
        r XTRIM mystream MAXLEN 1
        set pending [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending] 3
        assert_equal [lindex $pending 0] {1-0 {} -1 1}
    }

    # Verify that XNACK handles more IDs than fit in the stack-allocated
    # static vector (STREAMID_STATIC_VECTOR_LEN), forcing a heap allocation
    # for the ID array. Uses 50 IDs to exceed the typical static limit.
    test "XNACK with IDs exceeding STREAMID_STATIC_VECTOR_LEN for heap allocation" {
        r DEL mystream
        r XGROUP CREATE mystream grp $ MKSTREAM

        set ids {}
        for {set i 1} {$i <= 50} {incr i} {
            r XADD mystream $i-0 f v$i
            lappend ids "$i-0"
        }
        r XREADGROUP GROUP grp c1 COUNT 50 STREAMS mystream >

        set result [r XNACK mystream grp FAIL IDS 50 {*}$ids]
        assert_equal $result 50

        set pending [r XPENDING mystream grp - + 100]
        assert_equal [llength $pending] 50
        foreach entry $pending {
            assert_equal [lindex $entry 1] {}
        }
    }

    # Verify that the RETRYCOUNT option overrides the delivery_count that
    # the mode would normally set. It takes precedence over FATAL (would
    # set LLONG_MAX), SILENT (would decrement), and FAIL (would keep).
    # Also works when applied to an already-NACKed entry.
    test "XNACK RETRYCOUNT overrides delivery_count" {
        # RETRYCOUNT overrides FATAL: count is 42 instead of LLONG_MAX
        r DEL mystream
        r XADD mystream 1-0 f v
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 STREAMS mystream >

        assert_equal 1 [r XNACK mystream grp FATAL IDS 1 1-0 RETRYCOUNT 42]
        set pending [r XPENDING mystream grp - + 10]
        assert_equal [lindex $pending 0] {1-0 {} -1 42}

        # RETRYCOUNT overrides SILENT: count is 10 instead of decrement
        r XCLAIM mystream grp c1 0 1-0
        assert_equal 1 [r XNACK mystream grp SILENT IDS 1 1-0 RETRYCOUNT 10]
        set pending [r XPENDING mystream grp - + 10]
        assert_equal [lindex $pending 0 3] 10

        # RETRYCOUNT 0: explicitly set count to zero
        r XCLAIM mystream grp c1 0 1-0
        assert_equal 1 [r XNACK mystream grp FAIL IDS 1 1-0 RETRYCOUNT 0]
        set pending [r XPENDING mystream grp - + 10]
        assert_equal [lindex $pending 0] {1-0 {} -1 0}

        # RETRYCOUNT on already-NACKed entry: overwrites the existing count
        assert_equal 1 [r XNACK mystream grp FAIL IDS 1 1-0 RETRYCOUNT 99]
        set pending [r XPENDING mystream grp - + 10]
        assert_equal [lindex $pending 0] {1-0 {} -1 99}
    }

    # Verify FORCE option behavior. FORCE creates an unowned PEL entry for an
    # ID that is not currently in any consumer's PEL, as long as the
    # corresponding stream entry exists. Covers:
    #  - Creating a new NACKed PEL entry without prior XREADGROUP.
    #  - Skipping non-existent stream entries (returns 0).
    #  - FATAL and SILENT modes apply their delivery_count logic on FORCE-created entries.
    #  - On already-owned entries, FORCE follows the normal NACK path.
    #  - On already-NACKed entries, FORCE is a no-op (found-path applies).
    #  - FORCE on an empty stream returns 0 and creates no PEL entry.
    test "XNACK FORCE behavior" {
        # FORCE creates a new unowned PEL entry (no prior XREADGROUP)
        r DEL mystream
        r XADD mystream 1-0 f v1
        r XADD mystream 2-0 f v2
        r XADD mystream 3-0 f v3
        r XGROUP CREATE mystream grp 0

        assert_equal 1 [r XNACK mystream grp FAIL IDS 1 1-0 FORCE]
        set pending [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending] 1
        # FAIL + FORCE on a new entry: delivery_count defaults to 0
        assert_equal [lindex $pending 0] {1-0 {} -1 0}
        # Verify the FORCE-created entry is claimable
        set claimed [r XCLAIM mystream grp c1 0 1-0]
        assert_equal [llength $claimed] 1
        assert_equal [lindex $claimed 0 0] 1-0

        # FORCE skips non-existent stream entries
        assert_equal 0 [r XNACK mystream grp FAIL IDS 1 9-9 FORCE]

        # FATAL + FORCE sets delivery_count to LLONG_MAX
        r XACK mystream grp 1-0
        assert_equal 1 [r XNACK mystream grp FATAL IDS 1 1-0 FORCE]
        set pending [r XPENDING mystream grp - + 10]
        assert_equal [lindex $pending 0] {1-0 {} -1 9223372036854775807}

        # SILENT + FORCE: no prior count to decrement, so clamped to 0
        r XACK mystream grp 1-0
        assert_equal 2 [r XNACK mystream grp SILENT IDS 2 1-0 2-0 FORCE]
        set pending [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending] 2
        assert_equal [lindex $pending 0] {1-0 {} -1 0}
        assert_equal [lindex $pending 1] {2-0 {} -1 0}

        # On already-owned PEL entries: FORCE follows the normal NACK path
        r XACK mystream grp 1-0 2-0
        r XREADGROUP GROUP grp c1 STREAMS mystream >
        assert_equal 3 [r XNACK mystream grp FAIL IDS 3 1-0 2-0 3-0 FORCE]
        set pending [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending] 3
        assert_equal [lindex $pending 0] {1-0 {} -1 1}
        assert_equal [lindex $pending 1] {2-0 {} -1 1}
        assert_equal [lindex $pending 2] {3-0 {} -1 1}
        set c1_pending [r XPENDING mystream grp - + 10 c1]
        assert_equal [llength $c1_pending] 0

        # On already-NACKed entry: found-path applies, no duplicate created
        assert_equal 1 [r XNACK mystream grp FAIL IDS 1 1-0 FORCE]
        set pending [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending] 3
        assert_equal [lindex $pending 0] {1-0 {} -1 1}

        # FORCE on empty stream (MKSTREAM group): entry doesn't exist, returns 0
        r DEL mystream
        r XGROUP CREATE mystream grp $ MKSTREAM
        assert_equal 0 [r XNACK mystream grp FAIL IDS 1 1-0 FORCE]
        set info [r XPENDING mystream grp]
        assert_equal [lindex $info 0] 0
    }

    # Verify that FORCE and RETRYCOUNT work together: FORCE creates the PEL
    # entry for IDs not currently in the PEL, and RETRYCOUNT overrides the
    # delivery_count that the mode would normally assign. Tests all three
    # modes (FAIL, SILENT, FATAL) combined with FORCE + RETRYCOUNT.
    test "XNACK FORCE + RETRYCOUNT combination" {
        r DEL mystream
        r XADD mystream 1-0 f v1
        r XADD mystream 2-0 f v2
        r XADD mystream 3-0 f v3
        r XGROUP CREATE mystream grp 0

        assert_equal 1 [r XNACK mystream grp FAIL IDS 1 1-0 RETRYCOUNT 7 FORCE]
        assert_equal 1 [r XNACK mystream grp SILENT IDS 1 2-0 RETRYCOUNT 5 FORCE]
        assert_equal 1 [r XNACK mystream grp FATAL IDS 1 3-0 RETRYCOUNT 99 FORCE]

        set pending [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending] 3

        # RETRYCOUNT overrides all modes: each has the explicitly set count
        assert_equal [lindex $pending 0] {1-0 {} -1 7}
        assert_equal [lindex $pending 1] {2-0 {} -1 5}
        assert_equal [lindex $pending 2] {3-0 {} -1 99}
    }

    # Verify that FORCE and RETRYCOUNT options are accepted both before and
    # after the "IDS numids id..." block, in any permutation.
    # Each sub-case ACKs the entry afterward so the next sub-case starts clean.
    test "XNACK flexible IDS position - options accepted before and after IDS block" {
        r DEL mystream
        r XADD mystream 1-0 f v1
        r XADD mystream 2-0 f v2
        r XADD mystream 3-0 f v3
        r XGROUP CREATE mystream grp 0

        # FORCE before IDS
        assert_equal 1 [r XNACK mystream grp FAIL FORCE IDS 1 1-0]
        r XACK mystream grp 1-0

        # FORCE + RETRYCOUNT both before IDS
        assert_equal 1 [r XNACK mystream grp FAIL FORCE RETRYCOUNT 42 IDS 1 1-0]
        r XACK mystream grp 1-0

        # RETRYCOUNT before IDS, FORCE after IDS
        assert_equal 1 [r XNACK mystream grp FAIL RETRYCOUNT 5 IDS 1 1-0 FORCE]
        r XACK mystream grp 1-0

        # FORCE before IDS, RETRYCOUNT after IDS
        assert_equal 1 [r XNACK mystream grp FAIL FORCE IDS 1 1-0 RETRYCOUNT 3]
        r XACK mystream grp 1-0

        # Multiple IDs with options before IDS
        assert_equal 3 [r XNACK mystream grp FAIL RETRYCOUNT 10 IDS 3 1-0 2-0 3-0 FORCE]
        r XACK mystream grp 1-0 2-0 3-0

        # Canonical order (IDS first, options after) still works
        assert_equal 1 [r XNACK mystream grp FAIL IDS 1 1-0 RETRYCOUNT 20 FORCE]
    }

    # Verify that re-NACKing an already-NACKed entry moves it to the tail
    # of the NACK zone. The NACK zone is time-ordered (FIFO insertion),
    # so moving to the tail means it will be claimed last.
    # Initial NACK order: 1-0, 2-0, 3-0. After re-NACKing 1-0 the order
    # becomes: 2-0, 3-0, 1-0. Verified via XREADGROUP CLAIM which walks
    # the PEL from pel_time_head to pel_time_tail.
    test "XNACK re-NACK moves entry to end of NACK zone" {
        r DEL mystream
        r XADD mystream 1-0 f v1
        r XADD mystream 2-0 f v2
        r XADD mystream 3-0 f v3
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 STREAMS mystream >

        r XNACK mystream grp FAIL IDS 3 1-0 2-0 3-0

        set info [r XINFO STREAM mystream FULL]
        set group [lindex [dict get $info groups] 0]
        assert_equal [dict get $group nacked-count] 3

        # Re-NACK 1-0 — moves it to end of NACK zone
        r XNACK mystream grp FAIL IDS 1 1-0

        set info [r XINFO STREAM mystream FULL]
        set group [lindex [dict get $info groups] 0]
        assert_equal [dict get $group nacked-count] 3
        assert_equal [dict get $group pel-count] 3

        # XREADGROUP CLAIM walks from pel_time_head to pel_time_tail.
        # After the re-NACK, zone order is: 2-0, 3-0, 1-0.
        # `after 10` ensures enough idle time for the CLAIM min-idle threshold.
        after 10
        set r1 [r XREADGROUP GROUP grp c2 COUNT 1 CLAIM 5 STREAMS mystream >]
        set msg1 [lindex [lindex $r1 0] 1 0 0]
        assert_equal $msg1 2-0

        after 10
        set r2 [r XREADGROUP GROUP grp c2 COUNT 1 CLAIM 5 STREAMS mystream >]
        set msg2 [lindex [lindex $r2 0] 1 0 0]
        assert_equal $msg2 3-0

        after 10
        set r3 [r XREADGROUP GROUP grp c2 COUNT 1 CLAIM 5 STREAMS mystream >]
        set msg3 [lindex [lindex $r3 0] 1 0 0]
        assert_equal $msg3 1-0
    }

    # Verify that NACKed entries are claimable by all three claim mechanisms.
    # NACKed entries have delivery_time=0 which means effectively infinite
    # idle time, so they always satisfy any min-idle-time threshold.
    # Each sub-test sets up a fresh stream, NACKs an entry, then claims it.
    test "XNACK NACKed entries claimable via XCLAIM, XAUTOCLAIM, and XREADGROUP CLAIM" {
        # XCLAIM with large min-idle-time: succeeds because idle is infinite
        r DEL mystream
        r XADD mystream 1-0 f v1
        r XADD mystream 2-0 f v2
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 STREAMS mystream >
        r XNACK mystream grp FAIL IDS 1 1-0
        set claimed [r XCLAIM mystream grp c2 99999 1-0]
        assert_equal [llength $claimed] 1
        assert_equal [lindex $claimed 0 0] 1-0
        set pending [r XPENDING mystream grp - + 10 c2]
        assert_equal [llength $pending] 1
        assert_equal [lindex $pending 0 0] 1-0

        # XAUTOCLAIM with large min-idle-time: also succeeds
        r DEL mystream
        r XADD mystream 1-0 f v1
        r XADD mystream 2-0 f v2
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 STREAMS mystream >
        r XNACK mystream grp FAIL IDS 1 1-0
        set result [r XAUTOCLAIM mystream grp c2 99999 0-0]
        set claimed_msgs [lindex $result 1]
        assert_equal [llength $claimed_msgs] 1
        assert_equal [lindex $claimed_msgs 0 0] 1-0

        # XCLAIM with min-idle-time 0: trivially satisfied
        r DEL mystream
        r XADD mystream 1-0 f v
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 STREAMS mystream >
        r XNACK mystream grp FAIL IDS 1 1-0
        set claimed [r XCLAIM mystream grp c2 0 1-0]
        assert_equal [llength $claimed] 1
        assert_equal [lindex $claimed 0 0] 1-0
        set pending [r XPENDING mystream grp - + 10 c2]
        assert_equal [llength $pending] 1
        assert_equal [lindex $pending 0 0] 1-0

        # XAUTOCLAIM with min-idle-time 0
        r DEL mystream
        r XADD mystream 1-0 f v
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 STREAMS mystream >
        r XNACK mystream grp FAIL IDS 1 1-0
        set result [r XAUTOCLAIM mystream grp c2 0 0-0]
        set claimed_msgs [lindex $result 1]
        assert_equal [llength $claimed_msgs] 1
        assert_equal [lindex $claimed_msgs 0 0] 1-0
        set pending [r XPENDING mystream grp - + 10 c2]
        assert_equal [llength $pending] 1
        assert_equal [lindex $pending 0 0] 1-0

        # XREADGROUP CLAIM: `after 10` ensures idle time exceeds the 5ms threshold
        r DEL mystream
        r XADD mystream 1-0 f v
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 STREAMS mystream >
        r XNACK mystream grp FAIL IDS 1 1-0
        after 10
        set result [r XREADGROUP GROUP grp c2 CLAIM 5 STREAMS mystream >]
        set pending [r XPENDING mystream grp - + 10 c2]
        assert_equal [llength $pending] 1
        assert_equal [lindex $pending 0 0] 1-0
    }

    # Verify that claiming a NACKed entry whose underlying stream data has
    # been deleted (a "ghost" PEL entry) cleans the PEL entry instead of
    # returning data.
    #  - XCLAIM on a deleted NACKed entry: returns empty, removes the PEL
    #    entry (exercises the streamPropagateXACK path for unowned NACKs).
    #  - XAUTOCLAIM: claims the surviving owned entry (2-0) and reports the
    #    deleted NACKed entry (3-0) in its deleted-IDs list.
    test "XNACK XCLAIM/XAUTOCLAIM of deleted NACKed entries cleans PEL" {
        r DEL mystream
        r XADD mystream 1-0 f v1
        r XADD mystream 2-0 f v2
        r XADD mystream 3-0 f v3
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 STREAMS mystream >

        r XNACK mystream grp FAIL IDS 2 1-0 3-0
        r XDEL mystream 1-0
        r XDEL mystream 3-0

        # XCLAIM of deleted unowned NACK: returns empty but cleans PEL
        # (exercises the streamPropagateXACK path for unowned NACKs)
        set claimed [r XCLAIM mystream grp c2 0 1-0]
        assert_equal [llength $claimed] 0

        # 1-0 was cleaned from PEL; 3-0 still a ghost NACKed entry
        set pending [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending] 2
        assert_match {2-0 c1 * 1} [lindex $pending 0]
        assert_equal [lindex $pending 1] {3-0 {} -1 1}

        # XAUTOCLAIM walks the entire PEL: claims surviving 2-0, reports deleted 3-0
        set result [r XAUTOCLAIM mystream grp c2 0 0-0]
        set claimed_msgs [lindex $result 1]
        set deleted_ids [lindex $result 2]

        assert_equal [llength $claimed_msgs] 1
        assert_equal [lindex $claimed_msgs 0 0] 2-0
        assert_equal [llength $deleted_ids] 1
        assert_equal [lindex $deleted_ids 0] 3-0

        set pending [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending] 1
        assert_match {2-0 c2 * 2} [lindex $pending 0]
    }

    # Verify that a client blocked on XREADGROUP BLOCK CLAIM is woken up
    # when entries are NACKed. NACKed entries have delivery_time=0 (infinite
    # idle), so they immediately satisfy the CLAIM min-idle-time threshold.
    # Uses a deferring client (non-blocking Tcl socket) to simulate a
    # blocked consumer waiting for claimable entries.
    test "XNACK XREADGROUP BLOCK CLAIM wakes up on NACKed entries" {
        r DEL mystream
        r XADD mystream 1-0 f v1
        r XADD mystream 2-0 f v2
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 STREAMS mystream >

        # c2 blocks waiting for claimable entries with min-idle 1000ms
        set rd [redis_deferring_client]
        $rd XREADGROUP GROUP grp c2 BLOCK 5000 CLAIM 1000 STREAMS mystream >
        wait_for_blocked_client

        # XNACK makes entries immediately claimable, waking c2
        r XNACK mystream grp FAIL IDS 2 1-0 2-0

        wait_for_blocked_clients_count 0
        set result [$rd read]
        assert_equal [llength $result] 1
        lassign [lindex $result 0] stream_name messages
        assert_equal $stream_name "mystream"
        assert_equal [llength $messages] 2
        assert_equal [lindex $messages 0 0] 1-0
        assert_equal [lindex $messages 1 0] 2-0

        # Entries are now owned by c2
        set pending [r XPENDING mystream grp - + 10 c2]
        assert_equal [llength $pending] 2
        assert_equal [lindex $pending 0 0] 1-0
        assert_equal [lindex $pending 1 0] 2-0

        $rd close
    }

    # Verify that when a consumer reads its own pending entries via
    # `XREADGROUP ... 0-0` (pending-entry scan), NACKed entries are
    # excluded because they are no longer owned by any consumer.
    # Only 2-0 (still owned by c1) should be returned.
    test "XNACK XREADGROUP pending read excludes NACKed entries" {
        r DEL mystream
        r XADD mystream 1-0 f v1
        r XADD mystream 2-0 f v2
        r XADD mystream 3-0 f v3
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 STREAMS mystream >

        r XNACK mystream grp FAIL IDS 2 1-0 3-0

        set result [r XREADGROUP GROUP grp c1 STREAMS mystream 0-0]
        set entries [lindex $result 0 1]
        assert_equal [llength $entries] 1
        assert_equal [lindex $entries 0 0] 2-0
    }

    # Verify that XINFO CONSUMERS reflects the reduced pending count after
    # XNACK, and that a consumer is not destroyed even when all its entries
    # are NACKed (0 pending). Consumer cleanup is only done by explicit
    # XGROUP DELCONSUMER.
    test "XNACK effect on consumer state and XINFO CONSUMERS" {
        r DEL mystream
        r XADD mystream 1-0 f v1
        r XADD mystream 2-0 f v2
        r XADD mystream 3-0 f v3
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 STREAMS mystream >

        # c1 initially has 3 pending
        set consumers [r XINFO CONSUMERS mystream grp]
        set c1_info [lindex $consumers 0]
        assert_equal [dict get $c1_info pending] 3

        # XNACK 2 entries: c1 pending drops to 1
        r XNACK mystream grp FAIL IDS 2 1-0 2-0
        set consumers [r XINFO CONSUMERS mystream grp]
        set c1_info [lindex $consumers 0]
        assert_equal [dict get $c1_info pending] 1

        # XNACK the last entry: c1 has 0 pending but still exists
        r XNACK mystream grp FAIL IDS 1 3-0
        set c1_pending [r XPENDING mystream grp - + 10 c1]
        assert_equal [llength $c1_pending] 0
        set info [r XINFO GROUPS mystream]
        set grp [lindex $info 0]
        assert_equal [dict get $grp consumers] 1
    }

    # Verify that XGROUP DESTROY removes all PEL entries including NACKed
    # (unowned) ones. After destroying the group and creating a new one,
    # the PEL is empty.
    test "XNACK XGROUP DESTROY cleans up NACKed entries" {
        r DEL mystream
        r XADD mystream 1-0 f v1
        r XADD mystream 2-0 f v2
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 STREAMS mystream >

        r XNACK mystream grp FAIL IDS 2 1-0 2-0

        set info [r XPENDING mystream grp]
        assert_equal [lindex $info 0] 2

        r XGROUP DESTROY mystream grp

        # New group has a clean PEL
        r XGROUP CREATE mystream grp2 0
        set info [r XPENDING mystream grp2]
        assert_equal [lindex $info 0] 0
    }

    # Verify that XGROUP DELCONSUMER only removes consumer-owned PEL entries.
    # NACKed (unowned) entries are not affected — they remain in the group
    # PEL and can still be claimed by other consumers.
    # Setup: c1 owns {1-0, 2-0}. NACK 1-0. Delete c1. Only 2-0 (owned)
    # is removed; 1-0 (NACKed/unowned) survives.
    test "XNACK XGROUP DELCONSUMER works when group PEL has NACKed entries" {
        r DEL mystream
        r XADD mystream 1-0 f v1
        r XADD mystream 2-0 f v2
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 STREAMS mystream >

        r XNACK mystream grp FAIL IDS 1 1-0

        # DELCONSUMER returns the count of consumer-owned entries removed (1: only 2-0)
        set deleted_pending [r XGROUP DELCONSUMER mystream grp c1]
        assert_equal $deleted_pending 1

        # Group PEL still has the NACKed entry (1-0)
        set info [r XPENDING mystream grp]
        assert_equal [lindex $info 0] 1

        set pending [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending] 1
        assert_equal [lindex $pending 0] {1-0 {} -1 1}

        set stream_info [r XINFO STREAM mystream FULL]
        set group [lindex [dict get $stream_info groups] 0]
        assert_equal [dict get $group nacked-count] 1

        # The surviving NACKed entry can still be claimed
        set claimed [r XCLAIM mystream grp c2 0 1-0]
        assert_equal [llength $claimed] 1
    }

    # Verify that the `nacked-count` field reported by XINFO STREAM FULL
    # accurately tracks the number of entries in the NACK zone through
    # various operations:
    #  - XNACK increases nacked-count (pel-count stays the same).
    #  - XCLAIM (reclaim) decreases nacked-count (moves entry back to owned).
    #  - XACK of a NACKed entry decreases both nacked-count and pel-count.
    #  - nacked-count is per-group (independent across groups).
    test "XNACK XINFO STREAM FULL nacked-count reflects nack zone size" {
        r DEL mystream
        r XADD mystream 1-0 f v1
        r XADD mystream 2-0 f v2
        r XADD mystream 3-0 f v3
        r XADD mystream 4-0 f v4
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 COUNT 4 STREAMS mystream >

        # Before any XNACK: all entries owned, nacked-count is 0
        set info [r XINFO STREAM mystream FULL]
        set group [lindex [dict get $info groups] 0]
        assert_equal [dict get $group nacked-count] 0
        assert_equal [dict get $group pel-count] 4

        # NACK one entry: nacked-count goes up, pel-count unchanged
        r XNACK mystream grp FAIL IDS 1 1-0
        set info [r XINFO STREAM mystream FULL]
        set group [lindex [dict get $info groups] 0]
        assert_equal [dict get $group nacked-count] 1
        assert_equal [dict get $group pel-count] 4

        # NACK two more
        r XNACK mystream grp FAIL IDS 2 2-0 3-0
        set info [r XINFO STREAM mystream FULL]
        set group [lindex [dict get $info groups] 0]
        assert_equal [dict get $group nacked-count] 3
        assert_equal [dict get $group pel-count] 4

        # Reclaim via XCLAIM: nacked-count decreases, pel-count unchanged
        r XCLAIM mystream grp c1 0 1-0
        set info [r XINFO STREAM mystream FULL]
        set group [lindex [dict get $info groups] 0]
        assert_equal [dict get $group nacked-count] 2
        assert_equal [dict get $group pel-count] 4

        # XACK a NACKed entry: both counts decrease
        r XACK mystream grp 2-0
        set info [r XINFO STREAM mystream FULL]
        set group [lindex [dict get $info groups] 0]
        assert_equal [dict get $group nacked-count] 1
        assert_equal [dict get $group pel-count] 3

        # XACK last NACKed entry: nacked-count reaches 0
        r XACK mystream grp 3-0
        set info [r XINFO STREAM mystream FULL]
        set group [lindex [dict get $info groups] 0]
        assert_equal [dict get $group nacked-count] 0
        assert_equal [dict get $group pel-count] 2

        # Multiple groups: nacked-count is per-group
        r XNACK mystream grp FAIL IDS 1 1-0
        r XGROUP CREATE mystream grp2 0
        r XREADGROUP GROUP grp2 c2 COUNT 4 STREAMS mystream >
        set info [r XINFO STREAM mystream FULL]
        set grp1 [lindex [dict get $info groups] 0]
        set grp2 [lindex [dict get $info groups] 1]
        assert_equal [dict get $grp1 nacked-count] 1
        assert_equal [dict get $grp2 nacked-count] 0
    }

    # Verify that NACKed entries survive an RDB save/reload cycle.
    # Uses all three modes (FAIL, FATAL, SILENT) plus FORCE-created entries
    # in a second group (grp2) with RETRYCOUNT. After DEBUG RELOAD:
    #  - delivery_counts are preserved (FAIL=1, FATAL=LLONG_MAX, SILENT=0).
    #  - NACK zone order is preserved (verified via XREADGROUP CLAIM order).
    #  - FORCE-created entries in grp2 are intact and claimable.
    test "XNACK RDB save and load preserves NACKed entries" {
        r DEL mystream
        r XADD mystream 1-0 f v1
        r XADD mystream 2-0 f v2
        r XADD mystream 3-0 f v3
        r XADD mystream 4-0 f v4
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 STREAMS mystream >

        # NACK with different modes
        r XNACK mystream grp FAIL IDS 1 1-0
        r XNACK mystream grp FATAL IDS 1 2-0
        r XNACK mystream grp SILENT IDS 1 3-0

        # Separate group: FORCE-created entries (no prior XREADGROUP in grp2)
        r XGROUP CREATE mystream grp2 0
        r XNACK mystream grp2 FAIL IDS 1 1-0 FORCE
        r XNACK mystream grp2 FATAL IDS 1 2-0 RETRYCOUNT 77 FORCE

        r SAVE
        r DEBUG RELOAD

        # Verify grp state after reload
        set pending [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending] 4
        assert_equal [lindex $pending 0] {1-0 {} -1 1}
        assert_equal [lindex $pending 1] {2-0 {} -1 9223372036854775807}
        assert_equal [lindex $pending 2] {3-0 {} -1 0}
        assert_match {4-0 c1 * 1} [lindex $pending 3]

        # Verify NACK zone order is preserved: 1-0, 2-0, 3-0
        set r1 [r XREADGROUP GROUP grp c2 COUNT 1 CLAIM 0 STREAMS mystream >]
        assert_equal [lindex [lindex $r1 0] 1 0 0] 1-0
        set r2 [r XREADGROUP GROUP grp c2 COUNT 1 CLAIM 0 STREAMS mystream >]
        assert_equal [lindex [lindex $r2 0] 1 0 0] 2-0

        # Verify grp2 FORCE-created entries survived the reload
        set pending2 [r XPENDING mystream grp2 - + 10]
        assert_equal [llength $pending2] 2
        assert_equal [lindex $pending2 0] {1-0 {} -1 0}
        assert_equal [lindex $pending2 1] {2-0 {} -1 77}

        set claimed [r XCLAIM mystream grp2 c1 0 1-0 2-0]
        assert_equal [llength $claimed] 2
    } {} {external:skip needs:debug}

    # Verify that NACKed entries survive DUMP/RESTORE serialization.
    # After DUMP + DEL + RESTORE, the PEL state (delivery_counts, unowned
    # status, nacked-count, and NACK zone claim order) is identical to the
    # original.
    test "XNACK NACKed entries survive DUMP and RESTORE" {
        r DEL mystream
        r XADD mystream 1-0 f v1
        r XADD mystream 2-0 f v2
        r XADD mystream 3-0 f v3
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 STREAMS mystream >

        r XNACK mystream grp SILENT IDS 1 1-0
        r XNACK mystream grp FATAL IDS 1 3-0

        set pending_before [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending_before] 3

        set dump [r DUMP mystream]
        r DEL mystream
        r RESTORE mystream 0 $dump

        # PEL state must match pre-DUMP state
        set pending_after [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending_after] 3

        assert_equal [lindex $pending_after 0] {1-0 {} -1 0}
        assert_match {2-0 c1 * 1} [lindex $pending_after 1]
        assert_equal [lindex $pending_after 2] {3-0 {} -1 9223372036854775807}

        set info [r XINFO STREAM mystream FULL]
        set group [lindex [dict get $info groups] 0]
        assert_equal [dict get $group nacked-count] 2

        # NACK zone claim order preserved: 1-0 first, then 3-0
        set r1 [r XREADGROUP GROUP grp c2 COUNT 1 CLAIM 0 STREAMS mystream >]
        assert_equal [lindex [lindex $r1 0] 1 0 0] 1-0
        set r2 [r XREADGROUP GROUP grp c2 COUNT 1 CLAIM 0 STREAMS mystream >]
        assert_equal [lindex [lindex $r2 0] 1 0 0] 3-0
    }

    # Verify that COPY creates an independent copy that preserves NACKed
    # entries (delivery_counts, unowned status, nacked-count, NACK zone
    # order). Also confirms the original stream is unaffected by operations
    # on the copy.
    # Uses hash-tag keys {t} to ensure same slot for cluster compatibility.
    test "XNACK COPY preserves NACKed entries" {
        r DEL mystream{t} mystream{t}_copy
        r XADD mystream{t} 1-0 f v1
        r XADD mystream{t} 2-0 f v2
        r XADD mystream{t} 3-0 f v3
        r XGROUP CREATE mystream{t} grp 0
        r XREADGROUP GROUP grp c1 STREAMS mystream{t} >

        r XNACK mystream{t} grp FAIL IDS 1 1-0
        r XNACK mystream{t} grp FATAL IDS 1 3-0

        r COPY mystream{t} mystream{t}_copy

        # Copied stream has the same NACKed state
        set pending [r XPENDING mystream{t}_copy grp - + 10]
        assert_equal [llength $pending] 3
        assert_equal [lindex $pending 0] {1-0 {} -1 1}
        assert_match {2-0 c1 * 1} [lindex $pending 1]
        assert_equal [lindex $pending 2] {3-0 {} -1 9223372036854775807}

        set info [r XINFO STREAM mystream{t}_copy FULL]
        set group [lindex [dict get $info groups] 0]
        assert_equal [dict get $group nacked-count] 2

        # NACK zone order is preserved in the copy
        set r1 [r XREADGROUP GROUP grp c2 COUNT 1 CLAIM 0 STREAMS mystream{t}_copy >]
        assert_equal [lindex [lindex $r1 0] 1 0 0] 1-0
        set r2 [r XREADGROUP GROUP grp c2 COUNT 1 CLAIM 0 STREAMS mystream{t}_copy >]
        assert_equal [lindex [lindex $r2 0] 1 0 0] 3-0

        # Original stream is unaffected: 1-0 still NACKed/unowned
        set orig_pending [r XPENDING mystream{t} grp - + 10]
        assert_equal [lindex $orig_pending 0 1] {}
    }
}

start_server {tags {"stream needs:debug"} overrides {appendonly yes aof-use-rdb-preamble no}} {
    # Verify that NACKed entries are correctly emitted during AOF rewrite
    # and fully restored via `debug loadaof`. After rewrite + reload,
    # delivery_counts, unowned status, and NACK zone claim order must
    # match the pre-rewrite state.
    test "XNACK entries survive AOF rewrite" {
        r DEL mystream
        r XADD mystream 1-0 f v1
        r XADD mystream 2-0 f v2
        r XADD mystream 3-0 f v3
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 STREAMS mystream >

        r XNACK mystream grp SILENT IDS 1 1-0
        r XNACK mystream grp FAIL IDS 1 3-0

        set pending_before [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending_before] 3
        assert_equal [lindex $pending_before 0] {1-0 {} -1 0}
        assert_match {2-0 c1 * 1} [lindex $pending_before 1]
        assert_equal [lindex $pending_before 2] {3-0 {} -1 1}

        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof

        # Verify state matches pre-rewrite
        set pending_after [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending_after] 3
        assert_equal [lindex $pending_after 0] {1-0 {} -1 0}
        assert_match {2-0 c1 * 1} [lindex $pending_after 1]
        assert_equal [lindex $pending_after 2] {3-0 {} -1 1}

        # NACK zone claim order preserved
        set r1 [r XREADGROUP GROUP grp c2 COUNT 1 CLAIM 0 STREAMS mystream >]
        assert_equal [lindex [lindex $r1 0] 1 0 0] 1-0
        set r2 [r XREADGROUP GROUP grp c2 COUNT 1 CLAIM 0 STREAMS mystream >]
        assert_equal [lindex [lindex $r2 0] 1 0 0] 3-0
    }

    # Test AOF rewrite when the NACK zone has more entries than the AOF
    # batch size (64 entries per XNACK FORCE batch in the AOF emitter).
    # With 65 NACKed entries + 1 owned entry, the rewriter must emit
    # multiple XNACK FORCE batches for the NACK zone and a separate
    # XCLAIM batch for the owned tail. After rewrite + reload, all 66
    # PEL entries must be intact with correct ownership and delivery_counts.
    test "XNACK AOF rewrite batch split -- 65 NACKed entries with owned tail" {
        r DEL mystream

        set total_nack 65
        set total [expr {$total_nack + 1}]

        for {set i 1} {$i <= $total} {incr i} {
            r XADD mystream $i-0 f v$i
        }
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 COUNT $total STREAMS mystream >

        set nack_ids {}
        for {set i 1} {$i <= $total_nack} {incr i} {
            lappend nack_ids $i-0
        }
        r XNACK mystream grp FAIL IDS $total_nack {*}$nack_ids

        set pending_before [r XPENDING mystream grp - + 200]
        assert_equal [llength $pending_before] $total

        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof

        set pending_after [r XPENDING mystream grp - + 200]
        assert_equal [llength $pending_after] $total

        # All 65 NACKed entries: unowned with delivery_count=1
        for {set i 0} {$i < $total_nack} {incr i} {
            set entry [lindex $pending_after $i]
            assert_equal [lindex $entry 0] "[expr {$i + 1}]-0"
            assert_equal [lindex $entry 1] {}
            assert_equal [lindex $entry 3] 1
        }

        # The last entry (66-0) is still owned by c1
        set last [lindex $pending_after $total_nack]
        assert_equal [lindex $last 0] "$total-0"
        assert_equal [lindex $last 1] c1

        set claimed [r XCLAIM mystream grp c2 0 1-0 65-0]
        assert_equal [llength $claimed] 2
    }

    # Edge case: the entire PEL consists of NACKed entries (no owned
    # entries at all). With 65 entries exceeding the 64-entry AOF batch
    # limit, the rewriter must split into multiple batches even though
    # there is no owned tail. After reload all entries are unowned.
    test "XNACK AOF rewrite batch split -- entire PEL is NACK zone" {
        r DEL mystream

        set total 65

        for {set i 1} {$i <= $total} {incr i} {
            r XADD mystream $i-0 f v$i
        }
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 COUNT $total STREAMS mystream >

        set nack_ids {}
        for {set i 1} {$i <= $total} {incr i} {
            lappend nack_ids $i-0
        }
        r XNACK mystream grp FAIL IDS $total {*}$nack_ids

        set pending_before [r XPENDING mystream grp - + 200]
        assert_equal [llength $pending_before] $total

        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof

        set pending_after [r XPENDING mystream grp - + 200]
        assert_equal [llength $pending_after] $total

        # Every entry is unowned with delivery_count=1
        for {set i 0} {$i < $total} {incr i} {
            assert_equal [lindex $pending_after $i] "[expr {$i + 1}]-0 {} -1 1"
        }
    }

    # Verify that AOF rewrite correctly batches NACKed entries that have
    # different delivery_counts. The AOF emitter groups consecutive entries
    # with the same delivery_count into a single XNACK FORCE command;
    # entries with different counts require separate batches.
    # Setup: 6 entries NACKed with mixed modes/RETRYCOUNT:
    #   1-0,2-0 = FATAL (LLONG_MAX), 3-0,4-0 = SILENT (0),
    #   5-0 = RETRYCOUNT 42, 6-0 = FAIL (1).
    test "XNACK AOF rewrite with mixed delivery_counts batches correctly" {
        r DEL mystream

        for {set i 1} {$i <= 6} {incr i} {
            r XADD mystream $i-0 f v$i
        }
        r XGROUP CREATE mystream grp 0
        r XREADGROUP GROUP grp c1 COUNT 6 STREAMS mystream >

        r XNACK mystream grp FATAL IDS 2 1-0 2-0
        r XNACK mystream grp SILENT IDS 2 3-0 4-0
        r XNACK mystream grp FAIL IDS 1 5-0 RETRYCOUNT 42
        r XNACK mystream grp FAIL IDS 1 6-0

        set pending_before [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending_before] 6

        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof

        set pending_after [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending_after] 6

        # Verify each entry retained its specific delivery_count
        foreach entry $pending_after {
            set id [lindex $entry 0]
            set consumer [lindex $entry 1]
            set dc [lindex $entry 3]

            assert_equal $consumer {}

            switch $id {
                1-0 - 2-0 {
                    assert_equal $dc 9223372036854775807
                }
                3-0 - 4-0 {
                    assert_equal $dc 0
                }
                5-0 {
                    assert_equal $dc 42
                }
                6-0 {
                    assert_equal $dc 1
                }
            }
        }
    }

    # Verify that FORCE-created PEL entries (which were never delivered
    # to a consumer via XREADGROUP) survive AOF rewrite. These entries
    # only exist in the group PEL, not in any consumer PEL, so the AOF
    # emitter must handle them specially.
    test "XNACK FORCE-created entries survive AOF rewrite" {
        r DEL mystream
        r XADD mystream 1-0 f v1
        r XADD mystream 2-0 f v2
        r XADD mystream 3-0 f v3
        r XGROUP CREATE mystream grp 0

        r XNACK mystream grp FAIL IDS 1 1-0 FORCE
        r XNACK mystream grp FATAL IDS 1 2-0 FORCE
        r XNACK mystream grp SILENT IDS 1 3-0 RETRYCOUNT 33 FORCE

        set pending_before [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending_before] 3

        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof

        set pending_after [r XPENDING mystream grp - + 10]
        assert_equal [llength $pending_after] 3
        assert_equal [lindex $pending_after 0] {1-0 {} -1 0}
        assert_equal [lindex $pending_after 1] {2-0 {} -1 9223372036854775807}
        assert_equal [lindex $pending_after 2] {3-0 {} -1 33}

        # FORCE-created entries are still claimable after reload
        set claimed [r XCLAIM mystream grp c1 0 1-0 2-0 3-0]
        assert_equal [llength $claimed] 3
    }
}

start_server {tags {"repl external:skip" "stream"}} {
    # Verify that XNACK commands replicate correctly to replicas.
    # Tests all three modes (FAIL, FATAL, SILENT) and FORCE option.
    # After wait_for_ofs_sync, the replica's PEL state must match the
    # master's: same delivery_counts, same unowned status.
    test "XNACK replication of modes and FORCE" {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        start_server {tags {"stream"}} {
            set replica [srv 0 client]

            $replica replicaof $master_host $master_port
            wait_for_sync $replica

            # Mode replication: FAIL, FATAL, SILENT on consumer-owned entries
            $master DEL mystream
            $master XADD mystream 1-0 f v1
            $master XADD mystream 2-0 f v2
            $master XADD mystream 3-0 f v3
            $master XADD mystream 4-0 f v4
            $master XGROUP CREATE mystream grp 0
            $master XREADGROUP GROUP grp c1 STREAMS mystream >
            wait_for_ofs_sync $master $replica

            $master XNACK mystream grp FAIL IDS 1 1-0
            $master XNACK mystream grp FATAL IDS 1 3-0
            $master XNACK mystream grp SILENT IDS 1 4-0
            wait_for_ofs_sync $master $replica

            # Verify replica state matches master
            set pending [$replica XPENDING mystream grp - + 10]
            assert_equal [llength $pending] 4
            assert_equal [lindex $pending 0] {1-0 {} -1 1}
            assert_match {2-0 c1 * 1} [lindex $pending 1]
            assert_equal [lindex $pending 2] {3-0 {} -1 9223372036854775807}
            assert_equal [lindex $pending 3] {4-0 {} -1 0}

            # FORCE replication: create PEL entries without prior XREADGROUP
            $master DEL mystream2
            $master XADD mystream2 1-0 f v1
            $master XADD mystream2 2-0 f v2
            $master XGROUP CREATE mystream2 grp 0
            wait_for_ofs_sync $master $replica

            $master XNACK mystream2 grp FAIL IDS 1 1-0 FORCE
            $master XNACK mystream2 grp FATAL IDS 1 2-0 FORCE
            wait_for_ofs_sync $master $replica

            set pending [$replica XPENDING mystream2 grp - + 10]
            assert_equal [llength $pending] 2

            assert_equal [lindex $pending 0] {1-0 {} -1 0}
            assert_equal [lindex $pending 1] {2-0 {} -1 9223372036854775807}
        }
    }
}

start_server {tags {"repl external:skip" "stream"}} {
    # Verify that reclaim/acknowledge operations on NACKed entries
    # propagate correctly to replicas. Tests four operations:
    #  1. XCLAIM a NACKed entry — replica sees new consumer ownership.
    #  2. XACK a NACKed entry — replica sees it removed from PEL.
    #  3. XAUTOCLAIM NACKed entries — replica sees new consumer ownership.
    #  4. XREADGROUP CLAIM NACKed entries — replica sees new consumer ownership.
    # Each step uses wait_for_ofs_sync to ensure replication completes
    # before reading from the replica.
    test "XNACK reclaim operations propagate correctly to replica" {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        start_server {tags {"stream"}} {
            set replica [srv 0 client]

            $replica replicaof $master_host $master_port
            wait_for_sync $replica

            $master DEL mystream
            $master XADD mystream 1-0 f v1
            $master XADD mystream 2-0 f v2
            $master XADD mystream 3-0 f v3
            $master XGROUP CREATE mystream grp 0
            $master XREADGROUP GROUP grp c1 STREAMS mystream >
            wait_for_ofs_sync $master $replica

            $master XNACK mystream grp FAIL IDS 2 1-0 2-0
            wait_for_ofs_sync $master $replica

            # 1. XCLAIM a NACKed entry: replica sees c2 owning 1-0
            $master XCLAIM mystream grp c2 0 1-0
            wait_for_ofs_sync $master $replica

            set pending [$replica XPENDING mystream grp - + 10 c2]
            assert_equal [llength $pending] 1
            assert_equal [lindex $pending 0 0] 1-0

            # 2. XACK a NACKed entry: 2-0 removed from replica PEL
            $master XACK mystream grp 2-0
            wait_for_ofs_sync $master $replica

            set all_pending [$replica XPENDING mystream grp - + 10]
            assert_equal [llength $all_pending] 2
            foreach entry $all_pending {
                assert {[lindex $entry 0] ne "2-0"}
            }

            # 3. XAUTOCLAIM NACKed entries: replica sees c3 owning 3-0
            $master XNACK mystream grp FAIL IDS 1 3-0
            wait_for_ofs_sync $master $replica

            $master XAUTOCLAIM mystream grp c3 99999 0-0
            wait_for_ofs_sync $master $replica

            set c3_pending [$replica XPENDING mystream grp - + 10 c3]
            assert_equal [llength $c3_pending] 1
            assert_equal [lindex $c3_pending 0 0] 3-0

            # 4. XREADGROUP CLAIM NACKed entries: replica sees c4 owning 1-0
            $master XNACK mystream grp FAIL IDS 1 1-0
            wait_for_ofs_sync $master $replica

            $master XREADGROUP GROUP grp c4 CLAIM 99999 STREAMS mystream >
            wait_for_ofs_sync $master $replica

            set c4_pending [$replica XPENDING mystream grp - + 10 c4]
            assert_equal [llength $c4_pending] 1
            assert_equal [lindex $c4_pending 0 0] 1-0
        }
    }
}

