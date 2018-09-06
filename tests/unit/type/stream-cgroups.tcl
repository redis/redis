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
}
