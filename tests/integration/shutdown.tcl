test {Shutting down master waits for replica to catch up} {
    start_server {overrides {save {}}} {
        start_server {overrides {save {}}} {
            set master [srv -1 client]
            set master_host [srv -1 host]
            set master_port [srv -1 port]
            set master_pid [srv -1 pid]
            set replica [srv 0 client]
            set replica_pid [srv 0 pid]

            # Config master.
            $master config set repl-diskless-sync yes
            $master config set repl-diskless-sync-delay 1
            $master config set shutdown-timeout 120 ; # for slow CI machines

            # Config replica.
            $replica replicaof $master_host $master_port
            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started."
            }
            $replica config set repl-diskless-load swapdb

            # Preparation: Set k to 1 on both master and replica.
            $master set k 1
            wait_for_ofs_sync $master $replica

            # Pause the replica.
            exec kill -SIGSTOP $replica_pid
            after 10

            # Fill upp the replication socket buffers.
            set junk_size 100000
            for {set i 0} {$i < $junk_size} {incr i} {
                $master set "key.$i.junkjunkjunk" \
                    "value.$i.blablablablablablablablabla"
            }

            # Incr k and immediately shutdown master.
            $master incr k
            exec kill -SIGTERM $master_pid
            #catch {$master shutdown}
            #puts "Shutdown done."

            # Wake up replica and check if master has waited for it.
            after 1000
            exec kill -SIGCONT $replica_pid
            wait_for_condition 50 100 {
                [$replica get k] eq 2
            } else {
                fail "Master exited before replica could catch up."
            }
            assert_equal [expr $junk_size + 1] [$replica dbsize]
        }
    }
} {} {repl external:skip}
