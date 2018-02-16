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
}
