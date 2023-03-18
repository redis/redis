source tests/support/cli.tcl

start_server {tags {"wait network external:skip"}} {
start_server {} {
    set slave [srv 0 client]
    set slave_host [srv 0 host]
    set slave_port [srv 0 port]
    set slave_pid [srv 0 pid]
    set master [srv -1 client]
    set master_host [srv -1 host]
    set master_port [srv -1 port]

    test {Setup slave} {
        $slave slaveof $master_host $master_port
        wait_for_condition 50 100 {
            [s 0 master_link_status] eq {up}
        } else {
            fail "Replication not started."
        }
    }

    test {WAIT out of range timeout (milliseconds)} {
        # Timeout is parsed as milliseconds by getLongLongFromObjectOrReply().
        # Verify we get out of range message if value is behind LLONG_MAX
        # (decimal value equals to 0x8000000000000000)
         assert_error "*or out of range*" {$master wait 2 9223372036854775808}

         # expected to fail by later overflow condition after addition
         # of mstime(). (decimal value equals to 0x7FFFFFFFFFFFFFFF)
         assert_error "*timeout is out of range*" {$master wait 2 9223372036854775807}

         assert_error "*timeout is negative*" {$master wait 2 -1}
    }

    test {WAIT should acknowledge 1 additional copy of the data} {
        $master set foo 0
        $master incr foo
        $master incr foo
        $master incr foo
        assert {[$master wait 1 5000] == 1}
        assert {[$slave get foo] == 3}
    }

    test {WAIT should not acknowledge 2 additional copies of the data} {
        $master incr foo
        assert {[$master wait 2 1000] <= 1}
    }

    test {WAIT should not acknowledge 1 additional copy if slave is blocked} {
        exec kill -SIGSTOP $slave_pid
        $master set foo 0
        $master incr foo
        $master incr foo
        $master incr foo
        assert {[$master wait 1 1000] == 0}
        exec kill -SIGCONT $slave_pid
        assert {[$master wait 1 1000] == 1}
    }

    test {WAIT implicitly blocks on client pause since ACKs aren't sent} {
        exec kill -SIGSTOP $slave_pid
        $master multi
        $master incr foo
        $master client pause 10000 write
        $master exec
        assert {[$master wait 1 1000] == 0}
        $master client unpause
        exec kill -SIGCONT $slave_pid
        assert {[$master wait 1 1000] == 1}
    }

    test {WAIT replica multiple clients unblock - reuse last result} {
        set rd [redis_deferring_client -1]
        set rd2 [redis_deferring_client -1]

        exec kill -SIGSTOP $slave_pid

        $rd incr foo
        $rd read

        $rd2 incr foo
        $rd2 read

        $rd wait 1 0
        $rd2 wait 1 0
        wait_for_blocked_clients_count 2 100 10 -1

        exec kill -SIGCONT $slave_pid

        assert_equal [$rd read] {1}
        assert_equal [$rd2 read] {1}

        $rd ping
        assert_equal [$rd read] {PONG}
        $rd2 ping
        assert_equal [$rd2 read] {PONG}

        $rd close
        $rd2 close
    }
}}


tags {"wait aof network external:skip"} {
    start_server {overrides {appendonly {yes} auto-aof-rewrite-percentage {0}}} {
        set master [srv 0 client]

        test {WAITAOF local copy before fsync} {
            r config set appendfsync no
            $master incr foo
            assert_equal [$master waitaof 1 0 50] {0 0} ;# exits on timeout
            r config set appendfsync everysec
        }

        test {WAITAOF local copy everysec} {
            $master incr foo
            assert_equal [$master waitaof 1 0 0] {1 0}
        }

        test {WAITAOF local copy with appendfsync always} {
            r config set appendfsync always
            $master incr foo
            assert_equal [$master waitaof 1 0 0] {1 0}
            r config set appendfsync everysec
        }

        test {WAITAOF local wait and then stop aof} {
            set rd [redis_deferring_client]
            $rd incr foo
            $rd read
            $rd waitaof 1 0 0
            wait_for_blocked_client
            r config set appendonly no ;# this should release the blocked client as an error
            assert_error {ERR WAITAOF cannot be used when numlocal is set but appendonly is disabled.} {$rd read}
            $rd close
        }

        test {WAITAOF local on server with aof disabled} {
            $master incr foo
            assert_error {ERR WAITAOF cannot be used when numlocal is set but appendonly is disabled.} {$master waitaof 1 0 0}
        }

        $master config set appendonly yes
        waitForBgrewriteaof $master

        start_server {overrides {appendonly {yes} auto-aof-rewrite-percentage {0}}} {
            set master_host [srv -1 host]
            set master_port [srv -1 port]
            set replica [srv 0 client]
            set replica_host [srv 0 host]
            set replica_port [srv 0 port]
            set replica_pid [srv 0 pid]

            # make sure the master always fsyncs first (easier to test)
            $master config set appendfsync always
            $replica config set appendfsync no

            test {WAITAOF on demoted master gets unblocked with an error} {
                set rd [redis_deferring_client]
                $rd incr foo
                $rd read
                $rd waitaof 0 1 0
                wait_for_blocked_client
                $replica replicaof $master_host $master_port
                assert_error {UNBLOCKED force unblock from blocking operation,*} {$rd read}
                $rd close
            }

            wait_for_ofs_sync $master $replica

            test {WAITAOF replica copy before fsync} {
                $master incr foo
                assert_equal [$master waitaof 0 1 50] {1 0} ;# exits on timeout
            }
            $replica config set appendfsync everysec

            test {WAITAOF replica copy everysec} {
                $master incr foo
                assert_equal [$master waitaof 0 1 0] {1 1}
            }

            test {WAITAOF replica copy appendfsync always} {
                $replica config set appendfsync always
                $master incr foo
                assert_equal [$master waitaof 0 1 0] {1 1}
                $replica config set appendfsync everysec
            }

            test {WAITAOF replica copy if replica is blocked} {
                exec kill -SIGSTOP $replica_pid
                $master incr foo
                assert_equal [$master waitaof 0 1 50] {1 0} ;# exits on timeout
                exec kill -SIGCONT $replica_pid
                assert_equal [$master waitaof 0 1 0] {1 1}
            }

            test {WAITAOF replica multiple clients unblock - reuse last result} {
                set rd [redis_deferring_client -1]
                set rd2 [redis_deferring_client -1]

                exec kill -SIGSTOP $replica_pid

                $rd incr foo
                $rd read

                $rd2 incr foo
                $rd2 read

                $rd waitaof 0 1 0
                $rd2 waitaof 0 1 0
                wait_for_blocked_clients_count 2 100 10 -1

                exec kill -SIGCONT $replica_pid

                assert_equal [$rd read] {1 1}
                assert_equal [$rd2 read] {1 1}

                $rd ping
                assert_equal [$rd read] {PONG}
                $rd2 ping
                assert_equal [$rd2 read] {PONG}

                $rd close
                $rd2 close
            }

            test {WAITAOF on promoted replica} {
                $replica replicaof no one
                $replica incr foo
                assert_equal [$replica waitaof 1 0 0] {1 0}
            }

            test {WAITAOF master that loses a replica and backlog is dropped} {
                $master config set repl-backlog-ttl 1
                after 2000 ;# wait for backlog to expire
                $master incr foo
                assert_equal [$master waitaof 1 0 0] {1 0}
            }

            test {WAITAOF master without backlog, wait is released when the replica finishes full-sync} {
                set rd [redis_deferring_client -1]
                $rd incr foo
                $rd read
                $rd waitaof 0 1 0
                wait_for_blocked_client -1
                $replica replicaof $master_host $master_port
                assert_equal [$rd read] {1 1}
                $rd close
            }

            test {WAITAOF master isn't configured to do AOF} {
                $master config set appendonly no
                $master incr foo
                assert_equal [$master waitaof 0 1 0] {0 1}
            }

            test {WAITAOF replica isn't configured to do AOF} {
                $master config set appendonly yes
                waitForBgrewriteaof $master
                $replica config set appendonly no
                $master incr foo
                assert_equal [$master waitaof 1 0 0] {1 0}
            }

            test {WAITAOF both local and replica got AOF enabled at runtime} {
                $replica config set appendonly yes
                waitForBgrewriteaof $replica
                $master incr foo
                assert_equal [$master waitaof 1 1 0] {1 1}
            }

            test {WAITAOF master sends PING after last write} {
                $master config set repl-ping-replica-period 1
                $master incr foo
                after 1200 ;# wait for PING
                $master get foo
                assert_equal [$master waitaof 1 1 0] {1 1}
                $master config set repl-ping-replica-period 10
            }

            test {WAITAOF master client didn't send any write command} {
                $master config set repl-ping-replica-period 1
                set client [redis_client -1]
                after 1200 ;# wait for PING
                assert_equal [$master waitaof 1 1 0] {1 1}
                $client close
                $master config set repl-ping-replica-period 10
            }

            test {WAITAOF master client didn't send any command} {
                $master config set repl-ping-replica-period 1
                set client [redis [srv -1 "host"] [srv -1 "port"] 0 $::tls]
                after 1200 ;# wait for PING
                assert_equal [$master waitaof 1 1 0] {1 1}
                $client close
                $master config set repl-ping-replica-period 10
            }

            foreach fsync {no everysec always} {
                test "WAITAOF when replica switches between masters, fsync: $fsync" {
                    # test a case where a replica is moved from one master to the other
                    # between two replication streams with different offsets that should
                    # not be mixed. done to smoke-test race conditions with bio thread.
                    start_server {overrides {appendonly {yes} auto-aof-rewrite-percentage {0}}} {
                        start_server {overrides {appendonly {yes} auto-aof-rewrite-percentage {0}}} {
                            set master2 [srv -1 client]
                            set master2_host [srv -1 host]
                            set master2_port [srv -1 port]
                            set replica2 [srv 0 client]
                            set replica2_host [srv 0 host]
                            set replica2_port [srv 0 port]
                            set replica2_pid [srv 0 pid]

                            $replica2 replicaof $master2_host $master2_port
                            wait_for_ofs_sync $master2 $replica2

                            $master config set appendfsync $fsync
                            $master2 config set appendfsync $fsync
                            $replica config set appendfsync $fsync
                            $replica2 config set appendfsync $fsync
                            if {$fsync eq "no"} {
                                after 2000 ;# wait for any previous fsync to finish
                                # can't afford "no" on the masters
                                $master config set appendfsync always
                                $master2 config set appendfsync always
                            } elseif {$fsync eq "everysec"} {
                                after 990 ;# hoping to hit a race
                            }

                            # add some writes and block a client on each master
                            set rd [redis_deferring_client -3]
                            set rd2 [redis_deferring_client -1]
                            $rd set boo 11
                            $rd2 set boo 22
                            $rd read
                            $rd2 read
                            $rd waitaof 1 1 0
                            $rd2 waitaof 1 1 0

                            if {$fsync eq "no"} {
                                # since appendfsync is disabled in the replicas, the client
                                # will get released only with full sync
                                wait_for_blocked_client -1
                                wait_for_blocked_client -3
                            }
                            # switch between the two replicas
                            $replica2 replicaof $master_host $master_port
                            $replica replicaof $master2_host $master2_port
                            assert_equal [$rd read] {1 1}
                            assert_equal [$rd2 read] {1 1}
                            $rd close
                            $rd2 close

                            assert_equal [$replica get boo] 22
                            assert_equal [$replica2 get boo] 11
                        }
                    }
                }
            }
        }
    }
}

start_server {tags {"failover external:skip"}} {
start_server {} {
start_server {} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]

    set replica1 [srv -1 client]
    set replica1_pid [srv -1 pid]

    set replica2 [srv -2 client]

    test {setup replication for following tests} {
        $replica1 replicaof $master_host $master_port
        $replica2 replicaof $master_host $master_port
        wait_for_sync $replica1
        wait_for_sync $replica2
    }

    test {WAIT and WAITAOF replica multiple clients unblock - reuse last result} {
        set rd [redis_deferring_client]
        set rd2 [redis_deferring_client]

        $master config set appendonly yes
        $replica1 config set appendonly yes
        $replica2 config set appendonly yes

        $master config set appendfsync always
        $replica1 config set appendfsync no
        $replica2 config set appendfsync no

        waitForBgrewriteaof $master
        waitForBgrewriteaof $replica1
        waitForBgrewriteaof $replica2

        exec kill -SIGSTOP $replica1_pid

        $rd incr foo
        $rd read
        $rd waitaof 0 1 0

        # rd2 has a newer repl_offset
        $rd2 incr foo
        $rd2 read
        $rd2 wait 2 0

        wait_for_blocked_clients_count 2

        exec kill -SIGCONT $replica1_pid

        # WAIT will unblock the client first.
        assert_equal [$rd2 read] {2}

        # Make $replica1 catch up the repl_aof_off, then WAITAOF will unblock the client.
        $replica1 config set appendfsync always
        $master incr foo
        assert_equal [$rd read] {1 1}

        $rd ping
        assert_equal [$rd read] {PONG}
        $rd2 ping
        assert_equal [$rd2 read] {PONG}

        $rd close
        $rd2 close
    }
}
}
}
