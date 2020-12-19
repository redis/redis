start_server {tags {"pause"}} {
    test "Test read commands are not blocked by client pause" {
        r client PAUSE 100000000 READONLY
        set rd [redis_deferring_client]
        $rd GET FOO
        assert_equal [s 0 blocked_clients] 0
        r client unpause
    }

    test "Test write commands are paused by RO" {
        r client PAUSE 100000000 READONLY

        set rd [redis_deferring_client]
        $rd SET FOO BAR
        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {1}
        } else {
            fail "Clients are not blocked"
        }

        r client unpause
        assert_match "OK" [$rd read]
    }

    test "Test special commands are paused by RO" {
        r PFADD pause-hll test
        r client PAUSE 100000000 READONLY

        # Test that pfcount, which can replicate, is also blocked
        set rd [redis_deferring_client]
        $rd PFCOUNT pause-hll
        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {1}
        } else {
            fail "Clients are not blocked"
        }
        r client unpause 
        assert_match "1" [$rd read]
    }

    test "Test mutli-execs are blocked by pause RO" {
        r client PAUSE 100000000 READONLY
        set rd [redis_deferring_client]
        $rd MULTI
        assert_equal [$rd read] "OK"
        $rd PING
        assert_equal [$rd read] "QUEUED"
        $rd EXEC
        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {1}
        } else {
            fail "Clients are not blocked"
        }
        r client unpause 
        assert_match "PONG" [$rd read]
    }

    # Make sure we unpause at the end
    r client unpause
}