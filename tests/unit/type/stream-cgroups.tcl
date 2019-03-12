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

    test {XGROUP CREATE: automatic stream creation fails without MKSTREAM} {
        r DEL mystream
        catch {r XGROUP CREATE mystream mygroup $} err
        set err
    } {ERR*}

    test {XGROUP CREATE: automatic stream creation works with MKSTREAM} {
        r DEL mystream
        r XGROUP CREATE mystream mygroup $ MKSTREAM
    } {OK}

    test {XREADGROUP will return only new elements} {
        r XADD mystream * a 1
        r XADD mystream * b 2
        # XREADGROUP should return only the new elements "a 1" "b 1"
        # and not the element "foo bar" which was pre existing in the
        # stream (see previous test)
        set reply [
            r XREADGROUP GROUP mygroup client-1 STREAMS mystream ">"
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
            r XREADGROUP GROUP mygroup client-2 STREAMS mystream ">"
        ]
        assert {[llength [lindex $reply 0 1]] == 2}
        assert {[lindex $reply 0 1 0 1] eq {c 3}}

        set r1 [r XREADGROUP GROUP mygroup client-1 COUNT 10 STREAMS mystream 0]
        set r2 [r XREADGROUP GROUP mygroup client-2 COUNT 10 STREAMS mystream 0]
        assert {[lindex $r1 0 1 0 1] eq {a 1}}
        assert {[lindex $r2 0 1 0 1] eq {c 3}}
    }

    test {XPENDING is able to return pending items} {
        set pending [r XPENDING mystream mygroup - + 10]
        assert {[llength $pending] == 4}
        for {set j 0} {$j < 4} {incr j} {
            set item [lindex $pending $j]
            if {$j < 2} {
                set owner client-1
            } else {
                set owner client-2
            }
            assert {[lindex $item 1] eq $owner}
            assert {[lindex $item 1] eq $owner}
        }
    }

    test {XPENDING can return single consumer items} {
        set pending [r XPENDING mystream mygroup - + 10 client-1]
        assert {[llength $pending] == 2}
    }

    test {XACK is able to remove items from the client/group PEL} {
        set pending [r XPENDING mystream mygroup - + 10 client-1]
        set id1 [lindex $pending 0 0]
        set id2 [lindex $pending 1 0]
        assert {[r XACK mystream mygroup $id1] eq 1}
        set pending [r XPENDING mystream mygroup - + 10 client-1]
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

    test {XCLAIM can claim PEL items from another consumer} {
        # Add 3 items into the stream, and create a consumer group
        r del mystream
        set id1 [r XADD mystream * a 1]
        set id2 [r XADD mystream * b 2]
        set id3 [r XADD mystream * c 3]
        r XGROUP CREATE mystream mygroup 0

        # Client 1 reads item 1 from the stream without acknowledgements.
        # Client 2 then claims pending item 1 from the PEL of client 1
        set reply [
            r XREADGROUP GROUP mygroup client1 count 1 STREAMS mystream >
        ]
        assert {[llength [lindex $reply 0 1 0 1]] == 2}
        assert {[lindex $reply 0 1 0 1] eq {a 1}}
        r debug sleep 0.2
        set reply [
            r XCLAIM mystream mygroup client2 10 $id1
        ]
        assert {[llength [lindex $reply 0 1]] == 2}
        assert {[lindex $reply 0 1] eq {a 1}}

        # Client 1 reads another 2 items from stream
        r XREADGROUP GROUP mygroup client1 count 2 STREAMS mystream >
        r debug sleep 0.2

        # Delete item 2 from the stream. Now client 1 has PEL that contains
        # only item 3. Try to use client 2 to claim the deleted item 2
        # from the PEL of client 1, this should return nil
        r XDEL mystream $id2
        set reply [
            r XCLAIM mystream mygroup client2 10 $id2
        ]
        assert {[llength $reply] == 1}
        assert_equal "" [lindex $reply 0]

        # Delete item 3 from the stream. Now client 1 has PEL that is empty.
        # Try to use client 2 to claim the deleted item 3 from the PEL
        # of client 1, this should return nil
        r debug sleep 0.2
        r XDEL mystream $id3
        set reply [
            r XCLAIM mystream mygroup client2 10 $id3
        ]
        assert {[llength $reply] == 1}
        assert_equal "" [lindex $reply 0]
    }

    test {XCLAIM without JUSTID increments delivery count} {
        # Add 3 items into the stream, and create a consumer group
        r del mystream
        set id1 [r XADD mystream * a 1]
        set id2 [r XADD mystream * b 2]
        set id3 [r XADD mystream * c 3]
        r XGROUP CREATE mystream mygroup 0

        # Client 1 reads item 1 from the stream without acknowledgements.
        # Client 2 then claims pending item 1 from the PEL of client 1
        set reply [
            r XREADGROUP GROUP mygroup client1 count 1 STREAMS mystream >
        ]
        assert {[llength [lindex $reply 0 1 0 1]] == 2}
        assert {[lindex $reply 0 1 0 1] eq {a 1}}
        r debug sleep 0.2
        set reply [
            r XCLAIM mystream mygroup client2 10 $id1
        ]
        assert {[llength [lindex $reply 0 1]] == 2}
        assert {[lindex $reply 0 1] eq {a 1}}

        set reply [
            r XPENDING mystream mygroup - + 10
        ]
        assert {[llength [lindex $reply 0]] == 4}
        assert {[lindex $reply 0 3] == 2}

        # Client 3 then claims pending item 1 from the PEL of client 2 using JUSTID
        r debug sleep 0.2
        set reply [
            r XCLAIM mystream mygroup client3 10 $id1 JUSTID
        ]
        assert {[llength $reply] == 1}
        assert {[lindex $reply 0] eq $id1}

        set reply [
            r XPENDING mystream mygroup - + 10
        ]
        assert {[llength [lindex $reply 0]] == 4}
        assert {[lindex $reply 0 3] == 2}
    }

    start_server {} {
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

                # The consumed enty should be the third
                set myentry [lindex $item 0 1 0 1]
                assert {$myentry eq {a 3}}
            }
        }
    }
}
