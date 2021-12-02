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
            $master config set shutdown-timeout 300 ; # 5min for slow CI

            # Config replica.
            $replica replicaof $master_host $master_port
            $replica config set repl-diskless-load swapdb
            wait_for_sync $replica

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

            # Wake up replica and check if master has waited for it.
            after 1000
            exec kill -SIGCONT $replica_pid
            wait_for_condition 300 1000 {
                [$replica get k] eq 2
            } else {
                fail "Master exited before replica could catch up."
            }
            assert_equal [expr $junk_size + 1] [$replica dbsize]

            # Check shutdown log messages on master
            wait_for_log_messages -1 {"*ready to exit, bye bye*"} 0 50 100
            assert_equal 0 [count_log_message -1 "*Lagging replica*"]
            verify_log_message -1 "*1 of 1 replicas are in sync*" 0
        }
    }
} {} {repl external:skip}

test {Shutting down master waits for replica timeout} {
    start_server {} {
        start_server {} {
            set master [srv -1 client]
            set master_host [srv -1 host]
            set master_port [srv -1 port]
            set master_pid [srv -1 pid]
            set replica [srv 0 client]
            set replica_pid [srv 0 pid]

            # Config master.
            $master config set shutdown-timeout 2 ; # seconds

            # Config replica.
            $replica replicaof $master_host $master_port
            wait_for_sync $replica

            # Preparation: Set k to 1 on both master and replica.
            $master set k 1
            wait_for_ofs_sync $master $replica

            # Pause the replica.
            exec kill -SIGSTOP $replica_pid
            after 10

            # Incr k and immediately shutdown master.
            $master incr k
            exec kill -SIGTERM $master_pid

            # Wake up replica after master has finished shutting down.
            after 2500
            exec kill -SIGCONT $replica_pid

            # Check shutdown log messages on master
            verify_log_message -1 "*Lagging replica*" 0
            verify_log_message -1 "*0 of 1 replicas are in sync*" 0
        }
    }
} {} {repl external:skip}
