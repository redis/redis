start_server {tags {"pause"}} {
    test "Test read commands are not blocked by client pause" {
        r client PAUSE 100000000 WRITE
        set rd [redis_deferring_client]
        $rd GET FOO
        $rd PING
        $rd INFO
        assert_equal [s 0 blocked_clients] 0
        r client unpause
    }

    test "Test write commands are paused by RO" {
        r client PAUSE 100000000 WRITE

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
        r client PAUSE 100000000 WRITE

        # Test that pfcount, which can replicate, is also blocked
        set rd [redis_deferring_client]
        $rd PFCOUNT pause-hll
        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {1}
        } else {
            fail "Clients are not blocked"
        }

        # Test that publish, which adds the message to the replication
        # stream is blocked.
        set rd2 [redis_deferring_client]
        $rd2 publish foo bar
        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {2}
        } else {
            fail "Clients are not blocked"
        }

        # Test that SCRIPT LOAD, which is replicated. 
        set rd3 [redis_deferring_client]
        $rd3 script load "return 1"
        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {3}
        } else {
            fail "Clients are not blocked"
        }

        r client unpause 
        assert_match "1" [$rd read]
        assert_match "0" [$rd2 read]
        assert_match "*" [$rd3 read]
    }

    test "Test read/admin mutli-execs are not blocked by pause RO" {
        r SET FOO BAR
        r client PAUSE 100000000 WRITE
        set rd [redis_deferring_client]
        $rd MULTI
        assert_equal [$rd read] "OK"
        $rd PING
        assert_equal [$rd read] "QUEUED"
        $rd GET FOO
        assert_equal [$rd read] "QUEUED"
        $rd EXEC
        assert_equal [s 0 blocked_clients] 0
        r client unpause 
        assert_match "PONG BAR" [$rd read]
    }

    test "Test write mutli-execs are blocked by pause RO" {
        set rd [redis_deferring_client]
        $rd MULTI
        assert_equal [$rd read] "OK"
        $rd SET FOO BAR
        r client PAUSE 100000000 WRITE
        assert_equal [$rd read] "QUEUED"
        $rd EXEC
        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {1}
        } else {
            fail "Clients are not blocked"
        }
        r client unpause 
        assert_match "OK" [$rd read]
    }

    test "Test scripts are blocked by pause RO" {
        r client PAUSE 100000000 WRITE
        set rd [redis_deferring_client]
        $rd EVAL "return 1" 0
        
        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {1}
        } else {
            fail "Clients are not blocked"
        }
        r client unpause 
        assert_match "1" [$rd read]
    }

    test "Test multiple clients can be queued up and unblocked" {
        r client PAUSE 100000000 WRITE
        set clients [list [redis_deferring_client] [redis_deferring_client] [redis_deferring_client]]
        foreach client $clients {
            $client SET FOO BAR
        }
        
        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {3}
        } else {
            fail "Clients are not blocked"
        }
        r client unpause
        foreach client $clients {
            assert_match "OK" [$client read]
        }
    }

    test "Test clients with syntax errors will get responses immediately" {
        r client PAUSE 100000000 WRITE
        catch {r set FOO} err
        assert_match "ERR wrong number of arguments for *" $err
        r client unpause
    }

    # Make sure we unpause at the end
    r client unpause
}
