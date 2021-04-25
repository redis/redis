start_server {tags {"repl"}} {
    start_server {} {
        set master [srv -1 client]
        set master_host [srv -1 host]
        set master_port [srv -1 port]
        set replica [srv 0 client]

        test {First server should have role slave after SLAVEOF} {
            $replica slaveof $master_host $master_port
            wait_for_condition 50 100 {
                [s 0 role] eq {slave}
            } else {
                fail "Replication not started."
            }
            wait_for_sync $replica
        }

        test {Data corruption of replication forces full sync} {            
            set initial_psyncs [s -1 sync_partial_ok]
            set initial_syncs [s -1 sync_full]
            $master set foo bar
            # Test a multi-exec, since this is a common flow for how
            # data is replicated to replicas.
            $master multi
            $master debug replicate fake-command
            $master debug replicate set foo baz
            $master ping
            $master exec
            wait_for_sync $replica

            # Check for error message
            assert_equal [count_log_message 0 "== CRITICAL == This replica is sending an error to its master"] 1

            # Check for poison pill
            assert_equal [$replica get foo] "bar"

            # Make sure we did a full sync
            assert_equal [expr [s -1 sync_partial_ok] - $initial_psyncs] 0
            assert_equal [expr [s -1 sync_full] - $initial_syncs] 1
        }
    }
}
