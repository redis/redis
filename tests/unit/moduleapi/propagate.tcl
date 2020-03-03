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
                    ([$replica get thread] eq "10")
                } else {
                    fail "The two counters don't match the expected value."
                }
            }
        }
    }
}
