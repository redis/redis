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
}}


tags {"wait aof network external:skip"} {
    start_server {overrides {appendonly {yes} auto-aof-rewrite-percentage {0}}} {

        test {WAITAOF local copy everysec} {
            r set foo 0
            r incr foo
            assert_equal [$master waitaof 1 0 5000] {1 0}
        }

        test {WAITAOF local copy before fsync} {
            r config set appendfsync no
            r incr foo
            assert_equal [$master waitaof 1 0 50] {0 0}
            r config set appendfsync everysec
        }

        test {WAITAOF local copy with appendfsync always} {
            r config set appendfsync always
            r incr foo
            assert_equal [$master waitaof 1 0 5000] {1 0}
            r config set appendfsync everysec
        }

        start_server {overrides {appendonly {yes} auto-aof-rewrite-percentage {0}}} {
            set replica [srv 0 client]
            set replica_host [srv 0 host]
            set replica_port [srv 0 port]
            set replica_pid [srv 0 pid]
            set master [srv -1 client]
            set master_host [srv -1 host]
            set master_port [srv -1 port]

            # make sure the master always fsyncs first (easier to test)
            $master config set appendfsync always

            $replica replicaof $master_host $master_port
            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started."
            }

            test {WAITAOF replica copy everysec} {
                $master incr foo
                assert_equal [$master waitaof 0 1 5000] {1 1}
            }

            test {WAITAOF replica copy before fsync} {
                $replica config set appendfsync no
                $master incr foo
                assert_equal [$master waitaof 0 1 50] {1 0}
                $replica config set appendfsync everysec
            }

            test {WAITAOF replica copy appendfsync always} {
                $replica config set appendfsync always
                $master incr foo
                assert_equal [$master waitaof 0 1 5000] 1
                $replica config set appendfsync everysec
            }

            test {WAITAOF replica copy if replica is blocked} {
                exec kill -SIGSTOP $replica_pid
                $master incr foo
                assert {[$master wait 1 50] == 0}
                exec kill -SIGCONT $repliac_pid
                assert_equal [$master waitaof 0 1 5000] {1 1}
            }

            test {WAITAOF on promoted replica} {
                $replica replicaof no one
                $replica incr foo
                assert_equal [$replica waitaof 1 0 5000] {1 0}
            }

            test {WAITAOF master that loses a replica and backlog is dropped} {
                $master config set repl-backlog-ttl 1
                after 2000
                r incr foo
                assert_equal [$master waitaof 1 0 5000] {1 0}
            }

            test {WAITAOF master without backlog, wait is released when the replica joins} {
                $master incr foo
                $replica replicaof $master_host $master_port
                assert_equal [$master waitaof 0 1 5000] {1 1}
            }

            test {WAITAOF master isn't configured to do AOF} {
                $master config set appendonly no
                $master incr foo
                assert_equal [$master waitaof 0 1 5000] {0 1}
            }

            test {WAITAOF replica isn't configured to do AOF} {
                $master config set appendonly yes
                waitForBgrewriteaof $master
                replica config set appendonly no
                $master incr foo
                assert_equal [$master waitaof 1 0 5000] {1 0}
            }

            test {WAITAOF both local and replica got AOF enabled at runtime} {
                $replica config set appendonly yes
                waitForBgrewriteaof $replica
                $master incr foo
                assert_equal [$master waitaof 1 1 5000] {1 1}
            }
        }
    }
}
