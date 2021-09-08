start_server {tags {"pause network"}} {
    test "Test read commands are not blocked by client pause" {
        r client PAUSE 100000000 WRITE
        set rd [redis_deferring_client]
        $rd GET FOO
        $rd PING
        $rd INFO
        assert_equal [s 0 blocked_clients] 0
        r client unpause
        $rd close
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
        $rd close
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
        $rd close
        $rd2 close
        $rd3 close
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
        $rd close
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
        $rd close
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
        $rd close
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
            $client close
        }
    }

    test "Test clients with syntax errors will get responses immediately" {
        r client PAUSE 100000000 WRITE
        catch {r set FOO} err
        assert_match "ERR wrong number of arguments for *" $err
        r client unpause
    }

    test "Test both active and passive expiries are skipped during client pause" {
        set expired_keys [s 0 expired_keys]
        r multi
        r set foo bar PX 10
        r set bar foo PX 10
        r client PAUSE 100000000 WRITE
        r exec

        wait_for_condition 10 100 {
            [r get foo] == {} && [r get bar] == {}
        } else {
            fail "Keys were never logically expired"
        }

        # No keys should actually have been expired
        assert_match $expired_keys [s 0 expired_keys]

        r client unpause

        # Force the keys to expire
        r get foo
        r get bar

        # Now that clients have been unpaused, expires should go through
        assert_match [expr $expired_keys + 2] [s 0 expired_keys]   
    }

    test "Test that client pause starts at the end of a transaction" {
        r MULTI
        r SET FOO1 BAR
        r client PAUSE 100000000 WRITE
        r SET FOO2 BAR
        r exec

        set rd [redis_deferring_client]
        $rd SET FOO3 BAR
        
        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {1}
        } else {
            fail "Clients are not blocked"
        }

        assert_match "BAR" [r GET FOO1]
        assert_match "BAR" [r GET FOO2]
        assert_match "" [r GET FOO3]

        r client unpause 
        assert_match "OK" [$rd read]
        $rd close
    }

    start_server {tags {needs:repl external:skip}} {
        set master [srv -1 client]
        set master_host [srv -1 host]
        set master_port [srv -1 port]

        # Avoid PINGs
        $master config set repl-ping-replica-period 3600
        r replicaof $master_host $master_port

        wait_for_condition 50 100 {
            [s master_link_status] eq {up}
        } else {
            fail "Replication not started."
        }

        test "Test when replica paused, offset would not grow" {
            $master set foo bar
            set old_master_offset [status $master master_repl_offset]

            wait_for_condition 50 100 {
                [s slave_repl_offset] == [status $master master_repl_offset]
            } else {
                fail "Replication offset not matched."
            }

            r client pause 100000 write
            $master set foo2 bar2

            # Make sure replica received data from master
            wait_for_condition 50 100 {
                [s slave_read_repl_offset] == [status $master master_repl_offset]
            } else {
                fail "Replication not work."
            }

            # Replica would not apply the write command
            assert {[s slave_repl_offset] == $old_master_offset}
            r get foo2
        } {}

        test "Test replica offset would grow after unpause" {
            r client unpause
            wait_for_condition 50 100 {
                [s slave_repl_offset] == [status $master master_repl_offset]
            } else {
                fail "Replication not continue."
            }
            r get foo2
        } {bar2}
    }

    # Make sure we unpause at the end
    r client unpause
}
