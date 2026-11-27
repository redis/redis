if {$::compression} {
    set overrides {io-threads 4 repl-compression 1 repl-rdb-channel no}
    start_server [list tags {repl external:skip} overrides $overrides] {
        start_server [list overrides $overrides] {
            set master [srv -1 client]
            set replica [srv 0 client]

            test "zstd replication compression survives idle gaps between writes" {
                $replica replicaof [srv -1 host] [srv -1 port]
                wait_for_sync $replica
                wait_replica_online $master

                # Each gap exceeds repl-compression-max-latency (100 ms), so each write starts a new frame.
                for {set i 0} {$i < 5} {incr i} {
                    $master set k$i v$i
                    after 300
                }
                wait_for_ofs_sync $master $replica

                verify_log_message -1 "*Initialized compression at level 1 for client*" 0
                assert_equal 0 [count_log_message -1 "zstd compress error"]
                assert_equal 1 [s -1 sync_full]
                for {set i 0} {$i < 5} {incr i} {
                    assert_equal v$i [$replica get k$i]
                }
            }
        }
    }
}
