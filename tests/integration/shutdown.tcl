# This test suite tests shutdown when there are lagging replicas connected.

# Fill up the OS socket send buffer for the replica connection 1M at a time.
# When the replication buffer memory increases beyond 2M (often after writing 4M
# or so), we assume it's because the OS socket send buffer can't swallow
# anymore.
proc fill_up_os_socket_send_buffer_for_repl {idx} {
    set i 0
    while {1} {
        incr i
        populate 1024 junk$i: 1024 $idx
        after 10
        set buf_size [s $idx mem_total_replication_buffers]
        if {$buf_size > 2*1024*1024} {
            break
        }
    }
}

foreach how {sigterm shutdown} {
    test "Shutting down master waits for replica to catch up ($how)" {
        start_server {} {
            start_server {} {
                set master [srv -1 client]
                set master_host [srv -1 host]
                set master_port [srv -1 port]
                set master_pid [srv -1 pid]
                set replica [srv 0 client]
                set replica_pid [srv 0 pid]

                # Config master.
                $master config set shutdown-timeout 300; # 5min for slow CI
                $master config set repl-backlog-size 1;  # small as possible
                $master config set hz 100;               # cron runs every 10ms

                # Config replica.
                $replica replicaof $master_host $master_port
                wait_for_sync $replica

                # Preparation: Set k to 1 on both master and replica.
                $master set k 1
                wait_for_ofs_sync $master $replica

                # Pause the replica.
                exec kill -SIGSTOP $replica_pid
                after 10

                # Fill up the OS socket send buffer for the replica connection
                # to prevent the following INCR from reaching the replica via
                # the OS.
                fill_up_os_socket_send_buffer_for_repl -1

                # Incr k and immediately shutdown master.
                $master incr k
                switch $how {
                    sigterm {
                        exec kill -SIGTERM $master_pid
                    }
                    shutdown {
                        set rd [redis_deferring_client -1]
                        $rd shutdown
                    }
                }
                wait_for_condition 50 100 {
                    [s -1 shutdown_in_milliseconds] > 0
                } else {
                    fail "Master not indicating ongoing shutdown."
                }

                # Wake up replica and check if master has waited for it.
                after 20; # 2 cron intervals
                exec kill -SIGCONT $replica_pid
                wait_for_condition 300 1000 {
                    [$replica get k] eq 2
                } else {
                    fail "Master exited before replica could catch up."
                }

                # Check shutdown log messages on master
                wait_for_log_messages -1 {"*ready to exit, bye bye*"} 0 100 500
                assert_equal 0 [count_log_message -1 "*Lagging replica*"]
                verify_log_message -1 "*1 of 1 replicas are in sync*" 0
            }
        }
    } {} {repl external:skip}
}

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
            $master config set shutdown-timeout 1; # second

            # Config replica.
            $replica replicaof $master_host $master_port
            wait_for_sync $replica

            # Preparation: Set k to 1 on both master and replica.
            $master set k 1
            wait_for_ofs_sync $master $replica

            # Pause the replica.
            exec kill -SIGSTOP $replica_pid
            after 10

            # Fill up the OS socket send buffer for the replica connection to
            # prevent the following INCR k from reaching the replica via the OS.
            fill_up_os_socket_send_buffer_for_repl -1

            # Incr k and immediately shutdown master.
            $master incr k
            exec kill -SIGTERM $master_pid
            wait_for_condition 50 100 {
                [s -1 shutdown_in_milliseconds] > 0
            } else {
                fail "Master not indicating ongoing shutdown."
            }

            # Let master finish shutting down and check log.
            wait_for_log_messages -1 {"*ready to exit, bye bye*"} 0 100 100
            verify_log_message -1 "*Lagging replica*" 0
            verify_log_message -1 "*0 of 1 replicas are in sync*" 0

            # Wake up replica.
            exec kill -SIGCONT $replica_pid
            assert_equal 1 [$replica get k]
        }
    }
} {} {repl external:skip}

test "Shutting down master waits for replica then fails" {
    start_server {} {
        start_server {} {
            set master [srv -1 client]
            set master_host [srv -1 host]
            set master_port [srv -1 port]
            set master_pid [srv -1 pid]
            set replica [srv 0 client]
            set replica_pid [srv 0 pid]

            # Config master and replica.
            $replica replicaof $master_host $master_port
            wait_for_sync $replica

            # Pause the replica and write a key on master.
            exec kill -SIGSTOP $replica_pid
            after 10
            $master incr k

            # Two clients call blocking SHUTDOWN in parallel.
            set rd1 [redis_deferring_client -1]
            set rd2 [redis_deferring_client -1]
            $rd1 shutdown
            $rd2 shutdown
            set info_clients [$master info clients]
            assert_match "*connected_clients:3*" $info_clients
            assert_match "*blocked_clients:2*" $info_clients

            # Start a very slow initial AOFRW, which will prevent shutdown.
            $master config set rdb-key-save-delay 30000000; # 30 seconds
            $master config set appendonly yes

            # Wake up replica, causing master to continue shutting down.
            exec kill -SIGCONT $replica_pid

            # SHUTDOWN returns an error to both clients blocking on SHUTDOWN.
            catch { $rd1 read } e1
            catch { $rd2 read } e2
            assert_match "*Errors trying to SHUTDOWN. Check logs*" $e1
            assert_match "*Errors trying to SHUTDOWN. Check logs*" $e2
            $rd1 close
            $rd2 close

            # Check shutdown log messages on master.
            verify_log_message -1 "*1 of 1 replicas are in sync*" 0
            verify_log_message -1 "*Writing initial AOF, can't exit*" 0
            verify_log_message -1 "*Errors trying to shut down*" 0

            # Let master to exit fast, without waiting for the very slow AOFRW.
            catch {$master shutdown nosave force}
        }
    }
} {} {repl external:skip}

test "Shutting down master waits for replica then aborted" {
    start_server {} {
        start_server {} {
            set master [srv -1 client]
            set master_host [srv -1 host]
            set master_port [srv -1 port]
            set master_pid [srv -1 pid]
            set replica [srv 0 client]
            set replica_pid [srv 0 pid]

            # Config master and replica.
            $replica replicaof $master_host $master_port
            wait_for_sync $replica

            # Pause the replica and write a key on master.
            exec kill -SIGSTOP $replica_pid
            after 10
            $master incr k

            # Two clients call blocking SHUTDOWN in parallel.
            set rd1 [redis_deferring_client -1]
            set rd2 [redis_deferring_client -1]
            $rd1 shutdown
            $rd2 shutdown
            set info_clients [$master info clients]
            assert_match "*connected_clients:3*" $info_clients
            assert_match "*blocked_clients:2*" $info_clients

            # Abort the shutdown
            $master shutdown abort

            # Wake up replica, causing master to continue shutting down.
            exec kill -SIGCONT $replica_pid

            # SHUTDOWN returns an error to both clients blocking on SHUTDOWN.
            catch { $rd1 read } e1
            catch { $rd2 read } e2
            assert_match "*Errors trying to SHUTDOWN. Check logs*" $e1
            assert_match "*Errors trying to SHUTDOWN. Check logs*" $e2
            $rd1 close
            $rd2 close

            # Check shutdown log messages on master.
            verify_log_message -1 "*Shutdown manually aborted*" 0
        }
    }
} {} {repl external:skip}
