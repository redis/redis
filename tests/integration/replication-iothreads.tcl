#
# Copyright (c) 2025-Present, Redis Ltd.
# All rights reserved.
#
# Licensed under your choice of (a) the Redis Source Available License 2.0
# (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
# GNU Affero General Public License v3 (AGPLv3).
#

# Tests for master and slave clients in IO threads

# Helper function to get master client IO thread from INFO replication
proc get_master_client_io_thread {r} {
    return [status $r master_client_io_thread]
}

# Helper function to get slave client IO thread from INFO replication
proc get_slave_client_io_thread {r slave_idx} {
    set info [$r info replication]
    set lines [split $info "\r\n"]

    foreach line $lines {
        if {[string match "slave${slave_idx}:*" $line]} {
            # Parse the slave line to extract io-thread value
            set parts [split $line ","]
            foreach part $parts {
                if {[string match "*io-thread=*" $part]} {
                    set kv [split $part "="]
                    assert_equal [llength $kv] 2
                    return [lindex $kv 1]
                }
            }
        }
    }
    return -1
}

start_server {overrides {io-threads 4 save ""} tags {"iothreads repl network external:skip"}} {
start_server {overrides {io-threads 4 save ""}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    set slave [srv -1 client]

    test {Setup slave} {
        $slave slaveof $master_host $master_port
        wait_for_condition 1000 100 {
            [s -1 master_link_status] eq {up}
        } else {
            fail "Replication not started."
        }
    }

    test {Master client moves to IO thread after sync complete} {
        # Check master client thread assignment (master client is on slave side)
        wait_for_condition 100 100 {
            [get_master_client_io_thread $slave] > 0
        } else {
            fail "Master client was not assigned to IO thread"
        }
    }

    test {Slave client assignment to IO threads} {
        # Verify slave is connected and online
        wait_replica_online $master 0

        # Slave client is connected - force a write so that it's assigned to an
        # IO thread.
        assert_equal "OK" [$master set x x]

        # Check slave client thread assignment
        wait_for_condition 50 100 {
            [get_slave_client_io_thread $master 0] > 0
        } else {
            fail "Slave client was not assigned to IO thread"
        }
    }

    test {WAIT command works with master/slave in IO threads} {
        # Test basic WAIT functionality
        $master set wait_test_key1 value1
        $master set wait_test_key2 value2
        $master incr wait_counter

        assert {[$master wait 1 2000] == 1}

        # Verify data reached slave
        wait_for_condition 10 100 {
            [$slave get wait_test_key1] eq "value1" &&
            [$slave get wait_test_key2] eq "value2" &&
            [$slave get wait_counter] eq "1"
        } else {
            fail "commands not propagated to IO thread slave in time"
        }
    }

    test {Replication data integrity with IO threads} {
        # Generate significant replication traffic
        for {set i 0} {$i < 100} {incr i} {
            $master set bulk_key_$i [string repeat "data" 10]
            $master lpush bulk_list element_$i
            $master zadd bulk_zset $i member_$i
            if {$i % 20 == 0} {
                # Periodically verify WAIT works
                assert {[$master wait 1 2000] == 1}
            }
        }

        # Final verification
        wait_for_condition 50 100 {
            [$slave get bulk_key_99] eq [string repeat "data" 10] &&
            [$slave llen bulk_list] eq 100 &&
            [$slave zcard bulk_zset] eq 100
        } else {
            fail "Replication data integrity failed"
        }
    }

    test {WAIT timeout behavior with slave in IO thread} {
        set slave_pid [srv -1 pid]

        # Pause slave to test timeout
        pause_process $slave_pid

        # Should timeout and return 0 acks
        $master set timeout_test_key timeout_value
        set start_time [clock milliseconds]
        assert {[$master wait 1 2000] == 0}
        set elapsed [expr {[clock milliseconds] - $start_time}]
        assert_range $elapsed 2000 2500

        # Resume and verify recovery
        resume_process $slave_pid

        assert {[$master wait 1 2000] == 1}

        # Verify data reached slave after resume
        wait_for_condition 10 100 {
            [$slave get timeout_test_key] eq "timeout_value"
        } else {
            fail "commands not propagated to IO thread slave in time"
        }
    }

    test {Network interruption recovery with IO threads} {
        # Generate traffic before interruption
        for {set i 0} {$i < 50} {incr i} {
            $master set pre_interrupt_$i value_$i
        }

        # Simulate network interruption
        pause_process $slave_pid

        # Continue writing during interruption
        for {set i 0} {$i < 50} {incr i} {
            $master set during_interrupt_$i value_$i
        }

        # WAIT should timeout
        assert {[$master wait 1 2000] == 0}

        # Resume slave and verify recovery
        resume_process $slave_pid

        # Verify WAIT works again
        assert {[$master wait 1 2000] == 1}

        # Wait for reconnection and catch up
        wait_for_condition 100 100 {
            [$slave get during_interrupt_49] eq "value_49"
        } else {
            fail "Slave didn't catch up after network recovery"
        }

        $master set post_recovery_test recovery_value
        wait_for_condition 10 100 {
          [$slave get post_recovery_test] eq "recovery_value"
        } else {
          fail "Slave didn't receive 'set post_recovery_test' command"
        }

        # Check thread assignments after recovery
        wait_for_condition 100 100 {
            [get_master_client_io_thread $slave] > 0
        } else {
            fail "Slave client not assigned to IO thread after recovery"
        }
    }

    test {Replication reconnection cycles with IO threads} {
        # Test multiple disconnect/reconnect cycles
        for {set cycle 0} {$cycle < 3} {incr cycle} {
            # Generate traffic
            for {set i 0} {$i < 20} {incr i} {
                $master set cycle_${cycle}_key_$i value_$i
            }

            assert {[$master wait 1 2000] == 1}

            # Record thread assignments during cycle
            set master_thread [get_master_client_io_thread $slave]
            set slave_thread [get_slave_client_io_thread $master 0]
            puts "Cycle $cycle - Master thread: $master_thread, Slave thread: $slave_thread"

            # Disconnect and reconnect (except last cycle)
            if {$cycle < 2} {
                $slave replicaof no one
                after 100
                $slave replicaof $master_host $master_port
                wait_for_sync $slave
            }
        }

        # Verify final state
        wait_for_condition 10 100 {
            [$slave get cycle_2_key_19] eq "value_19"
        } else {
            fail "last command not propagated to IO thread slave in time"
        }
    }

    test {INFO replication shows correct thread information} {
        # Test INFO replication output format
        set info [$master info replication]

        # Should show master role
        assert_match "*role:master*" $info

        # Should have slave thread information
        assert_match "*slave0:*io-thread=*" $info

        # Test we can parse the thread ID
        set slave_thread [get_slave_client_io_thread $master 0]
        assert_morethan $slave_thread 0

        # Test master client thread info
        set slave_info [$slave info replication]
        assert_match "*role:slave*" $slave_info
        assert_match "*master_client_io_thread:*" $slave_info

        set master_thread [get_master_client_io_thread $slave]
        assert_morethan $master_thread 0
    }
}
}

start_server {overrides {io-threads 4 save ""} tags {"iothreads repl network external:skip"}} {
start_server {overrides {io-threads 4 save ""}} {
start_server {overrides {io-threads 4 save ""}} {
start_server {overrides {io-threads 4 save ""}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    set slave1 [srv -1 client]
    set slave2 [srv -2 client]
    set slave3 [srv -3 client]

    test {Multiple slaves across IO threads} {
        # Setup replication for all slaves
        $slave1 replicaof $master_host $master_port
        $slave2 replicaof $master_host $master_port
        $slave3 replicaof $master_host $master_port

        # Wait for all slaves to be online
        wait_replica_online $master 0
        wait_replica_online $master 1
        wait_replica_online $master 2

        set iterations 5
        while {[incr iterations -1] >= 0} {
            # Slave clients are connected - force a write so that they are assigned
            # to IO threads.
            assert_equal "OK" [$master set x x]

            wait_for_condition 10 100 {
                ([get_slave_client_io_thread $master 0] > 0) &&
                ([get_slave_client_io_thread $master 1] > 0) &&
                ([get_slave_client_io_thread $master 2] > 0)
            } else {
                continue
            }

            break
        }
        if {$iterations < 0} {
            fail "Replicas failed to be assigned to IO threads in time"
        }

        # Test concurrent replication to all slaves
        for {set i 0} {$i < 200} {incr i} {
            $master set multi_key_$i value_$i
            if {$i % 50 == 0} {
                assert {[$master wait 3 2000] == 3}
            }
        }

        # Final verification all slaves got data
        wait_for_condition 50 100 {
            [$slave1 get multi_key_199] eq "value_199" &&
            [$slave2 get multi_key_199] eq "value_199" &&
            [$slave3 get multi_key_199] eq "value_199"
        } else {
            fail "Multi-slave replication failed"
        }
    }

    test {WAIT with multiple slaves in IO threads} {
        # Test various WAIT scenarios
        $master set wait_multi_test1 value1
        assert {[$master wait 3 2000] == 3}

        $master set wait_multi_test2 value2
        assert {[$master wait 2 2000] >= 2}

        $master set wait_multi_test3 value3
        assert {[$master wait 1 2000] >= 1}

        # Verify all slaves have the data
        wait_for_condition 10 100 {
            [$slave1 get wait_multi_test3] eq "value3" &&
            [$slave2 get wait_multi_test3] eq "value3" &&
            [$slave3 get wait_multi_test3] eq "value3"
        } else {
            fail "commands not propagated to io thread slaves in time"
        }
    }
}
}
}
}

