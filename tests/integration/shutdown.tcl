test {Shutting down master waits for replica to catch up} {
    start_server {tags {"repl external:skip"}} {
        start_server {} {
            set master [srv -1 client]
            set master_host [srv -1 host]
            set master_port [srv -1 port]
            set replica [srv 0 client]

            $master config set repl-diskless-sync yes
            $master config set repl-diskless-sync-delay 1

            $replica replicaof $master_host $master_port
            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started."
            }

            $replica config set repl-diskless-load swapdb

            $master set k 1
            wait_for_ofs_sync $master $replica
            # Stopping the replica for one second to check if the master waits.
            exec kill -SIGSTOP [srv 0 pid]
            after 100

            # Fill upp the replication socket buffers
            $master debug populate 10000000

            $master incr k

            # Shutdown master and make check that clients can't connect.
            puts "Shutting down master."
            catch {$master shutdown nosave}
            puts "Shutdown done."
            #catch {set rd [redis_deferring_client]} e
            #assert_match {*connection refused*} $e

            after 500
            exec kill -SIGCONT [srv 0 pid]
            puts [$replica get k]
            wait_for_condition 50 100 {
                !([puts [$replica get k]] eq 42) && \
                [$replica get k] eq 2
            } else {
                fail "Master exited before replica could catch up."
            }
            # wait_for_ofs_sync $master $replica
        }
    }
} {} {repl external:skip}
