set testmodule [file normalize tests/modules/propagate.so]

tags "modules" {
    test {Modules can propagate in async and threaded contexts} {
        start_server {} {
            set replica [srv 0 client]
            set replica_host [srv 0 host]
            set replica_port [srv 0 port]
            start_server [list overrides [list loadmodule "$testmodule"]] {
                set master [srv 0 client]
                set master_host [srv 0 host]
                set master_port [srv 0 port]

                # Start the replication process...
                $replica replicaof $master_host $master_port
                wait_for_sync $replica

                after 1000
                $master propagate-test

                wait_for_condition 5000 10 {
                    ([$replica get timer] eq "10") && \
                    ([$replica get a-from-thread] eq "10")
                } else {
                    fail "The two counters don't match the expected value."
                }

                $master propagate-test-2
                $master propagate-test-3
                $master multi
                $master propagate-test-2
                $master propagate-test-3
                $master exec
                wait_for_ofs_sync $master $replica

                assert_equal [s -1 unexpected_error_replies] 0
            }
        }
    }
}

tags "modules aof" {
    test {Modules RM_Replicate replicates MULTI/EXEC correctly} {
        start_server [list overrides [list loadmodule "$testmodule"]] {
            # Enable the AOF
            r config set appendonly yes
            r config set auto-aof-rewrite-percentage 0 ; # Disable auto-rewrite.
            waitForBgrewriteaof r

            r propagate-test-2
            r propagate-test-3
            r multi
            r propagate-test-2
            r propagate-test-3
            r exec

            # Load the AOF
            r debug loadaof

            assert_equal [s 0 unexpected_error_replies] 0
        }
    }
}
