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

# Regression test for https://github.com/redis/redis/issues/15311
#
# With io-threads enabled and a replica attached, a replica client can be handed
# back from an IO thread to the main thread (clearing CLIENT_IO_WRITE_ENABLED)
# while its write handler (sendReplyToClient) is still installed on the IO
# thread's event loop. Because writeToClient() returns early when
# CLIENT_IO_WRITE_ENABLED is cleared, the stale write handler is never removed.
# With level-triggered epoll, the writable socket keeps firing EPOLLOUT and the
# IO thread spins at 100% CPU forever, even after all load has stopped.
#
# To hit the race deterministically each cycle:
#   1. Stop the replica process so it stops reading.
#   2. Write a multi-MB burst on the master: this fills the master->replica
#      socket, so the master does a partial write and installs the replica's
#      write handler on its IO thread.
#   3. Wait longer than the 100ms IOThreadClientsCron period: while the handler
#      is installed, the cron hands the replica client back to the main thread
#      and clears CLIENT_IO_WRITE_ENABLED.
#   4. Resume the replica: the client is sent back to the IO thread and drained
#      via writeToClient(c,0), which never removes the now-stale handler.
#
# After the cycles we measure the master process CPU during a strictly idle
# window. A healthy server is near-idle; a server hitting the bug burns ~one
# full core indefinitely.

# Read total CPU ticks (utime + stime) consumed by all threads of a process,
# from /proc/<pid>/stat. Linux only.
proc proc_cpu_ticks {pid} {
    set fd [open "/proc/$pid/stat" r]
    set data [read $fd]
    close $fd
    # The 'comm' field (field 2) is wrapped in parens and may itself contain
    # spaces or parens, so parse everything after the last ')'. After that, the
    # remaining whitespace-separated fields begin at 'state' (field 3), so:
    #   utime = field 14 -> index 11, stime = field 15 -> index 12
    set rest [string range $data [expr {[string last ")" $data] + 1}] end]
    set fields [split [string trim $rest]]
    set utime [lindex $fields 11]
    set stime [lindex $fields 12]
    return [expr {$utime + $stime}]
}

# Returns the fraction of one CPU core used by the process over the given
# wall-clock window (e.g. 1.0 == one core fully pegged).
proc measure_proc_cpu_fraction {pid window_ms} {
    set hz 100 ;# USER_HZ, virtually always 100 on Linux
    set t0 [proc_cpu_ticks $pid]
    after $window_ms
    set t1 [proc_cpu_ticks $pid]
    set ticks [expr {$t1 - $t0}]
    return [expr {double($ticks) / ($hz * ($window_ms / 1000.0))}]
}

if {$::accurate && $::tcl_platform(platform) eq "unix" && $::tcl_platform(os) eq "Linux"} {

start_server {overrides {io-threads 4 save ""} tags {"iothreads repl network external:skip"}} {
start_server {overrides {io-threads 4 save ""}} {


    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    set master_pid [srv 0 pid]
    set slave [srv -1 client]
    set slave_pid [srv -1 pid]

    test {Setup replication and assign replica client to an IO thread} {
        $slave slaveof $master_host $master_port
        wait_for_sync $slave
        wait_replica_online $master 0

        # Generate a little traffic so the replica client is moved to an IO
        # thread (replicas only move there once they are ONLINE and sending).
        for {set i 0} {$i < 50} {incr i} { $master set warmup:$i $i }
        wait_for_condition 50 100 {
            [string match "*slave0:*io-thread=*" [$master info replication]] &&
            [lindex [split [lindex [regexp -inline {io-thread=(\d+)} [$master info replication]] 1]] 0] > 0
        } else {
            fail "replica client was not assigned to an IO thread"
        }
    }

    test {IO thread does not spin on EPOLLOUT after replica write load stops} {
        set triggered 0
        set max_sessions 15

        for {set session 0} {$session < $max_sessions && !$triggered} {incr session} {
            # Sustained high-rate, tiny-value write load (mirrors the report's
            # `redis-benchmark -d 8`). The high command rate keeps the main
            # thread continuously draining the replica via writeToClient(c,0)
            # in processClientsFromMainThread, which is the path that can strand
            # the replica's write handler.
            set loaders {}
            for {set i 0} {$i < 12} {incr i} {
                lappend loaders [start_write_load $master_host $master_port 8 "" 8]
            }

            # Cycle the replica between stopped and running. Each stall (kept
            # longer than the 100ms IOThreadClientsCron period) fills the
            # master->replica socket and lets the cron hand the replica client
            # back to the main thread while its write handler is still installed;
            # the brief resume then lets processClientsFromMainThread drain it
            # via writeToClient(c,0), which never removes the stale handler.
            set deadline [expr {[clock milliseconds] + 6000}]
            while {[clock milliseconds] < $deadline} {
                exec kill -SIGSTOP $slave_pid
                after 130
                exec kill -SIGCONT $slave_pid
                after 150
            }
            catch {exec kill -SIGCONT $slave_pid}

            # Stop all load and let the replica catch up. A stuck (spinning) IO
            # thread also fails to push the tail of the stream, so catching up is
            # best-effort; the CPU measurement below is the authoritative detector.
            foreach handle $loaders {
                stop_write_load $handle
            }
            wait_load_handlers_disconnected
            # Let the replica fully catch up so the next session starts synced.
            # A stuck (spinning) IO thread can't push the tail of the stream, so
            # this is best-effort; the CPU measurement below is the real detector.
            wait_for_condition 150 100 {
                [status $master master_repl_offset] eq [status $slave master_repl_offset]
            } else {
                puts "Session $session: replica did not catch up (offset gap [expr {[status $master master_repl_offset] - [status $slave master_repl_offset]}])"
            }
            after 500

            # Measure master CPU twice during a strictly idle window. A one-shot
            # backlog drain finishes between samples; a stranded write handler
            # keeps the IO thread pegged across both.
            set frac1 [measure_proc_cpu_fraction $master_pid 1000]
            set frac2 [measure_proc_cpu_fraction $master_pid 1000]
            puts "Session $session: master CPU during idle = [format %.2f $frac1] / [format %.2f $frac2] core(s)"

            if {$frac1 > 0.5 && $frac2 > 0.5} {
                set triggered 1
                puts "Session $session: spin detected"
            }
        }

        assert_equal 0 $triggered \
            "IO thread is spinning on EPOLLOUT during idle (issue #15311)"
    }
}
}

}
