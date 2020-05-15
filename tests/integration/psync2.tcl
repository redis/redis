start_server {tags {"psync2"}} {
start_server {} {
start_server {} {
start_server {} {
start_server {} {
    set master_id 0                 ; # Current master
    set start_time [clock seconds]  ; # Test start time
    set counter_value 0             ; # Current value of the Redis counter "x"

    # Config
    set debug_msg 0                 ; # Enable additional debug messages

    set no_exit 0                   ; # Do not exit at end of the test

    set duration 20                 ; # Total test seconds

    set genload 1                   ; # Load master with writes at every cycle

    set genload_time 5000           ; # Writes duration time in ms

    set disconnect 1                ; # Break replication link between random
                                      # master and slave instances while the
                                      # master is loaded with writes.

    set disconnect_period 1000      ; # Disconnect repl link every N ms.

    for {set j 0} {$j < 5} {incr j} {
        set R($j) [srv [expr 0-$j] client]
        set R_host($j) [srv [expr 0-$j] host]
        set R_port($j) [srv [expr 0-$j] port]
        if {$debug_msg} {puts "Log file: [srv [expr 0-$j] stdout]"}
    }

    set cycle 1
    while {([clock seconds]-$start_time) < $duration} {
        test "PSYNC2: --- CYCLE $cycle ---" {}
        incr cycle

        # Create a random replication layout.
        # Start with switching master (this simulates a failover).

        # 1) Select the new master.
        set master_id [randomInt 5]
        set used [list $master_id]
        test "PSYNC2: \[NEW LAYOUT\] Set #$master_id as master" {
            $R($master_id) slaveof no one
            $R($master_id) config set repl-ping-replica-period 1 ;# increse the chance that random ping will cause issues
            if {$counter_value == 0} {
                $R($master_id) set x $counter_value
            }
        }

        # 2) Attach all the slaves to a random instance
        while {[llength $used] != 5} {
            while 1 {
                set slave_id [randomInt 5]
                if {[lsearch -exact $used $slave_id] == -1} break
            }
            set rand [randomInt [llength $used]]
            set mid [lindex $used $rand]
            set master_host $R_host($mid)
            set master_port $R_port($mid)

            test "PSYNC2: Set #$slave_id to replicate from #$mid" {
                $R($slave_id) slaveof $master_host $master_port
            }
            lappend used $slave_id
        }

        # Wait for replicas to sync. so next loop won't get -LOADING error
        wait_for_condition 50 1000 {
            [status $R([expr {($master_id+1)%5}]) master_link_status] == "up" &&
            [status $R([expr {($master_id+2)%5}]) master_link_status] == "up" &&
            [status $R([expr {($master_id+3)%5}]) master_link_status] == "up" &&
            [status $R([expr {($master_id+4)%5}]) master_link_status] == "up"
        } else {
            fail "Replica not reconnecting"
        }

        # 3) Increment the counter and wait for all the instances
        # to converge.
        test "PSYNC2: cluster is consistent after failover" {
            $R($master_id) incr x; incr counter_value
            for {set j 0} {$j < 5} {incr j} {
                wait_for_condition 50 1000 {
                    [$R($j) get x] == $counter_value
                } else {
                    fail "Instance #$j x variable is inconsistent"
                }
            }
        }

        # 4) Generate load while breaking the connection of random
        # slave-master pairs.
        test "PSYNC2: generate load while killing replication links" {
            set t [clock milliseconds]
            set next_break [expr {$t+$disconnect_period}]
            while {[clock milliseconds]-$t < $genload_time} {
                if {$genload} {
                    $R($master_id) incr x; incr counter_value
                }
                if {[clock milliseconds] == $next_break} {
                    set next_break \
                        [expr {[clock milliseconds]+$disconnect_period}]
                    set slave_id [randomInt 5]
                    if {$disconnect} {
                        $R($slave_id) client kill type master
                        if {$debug_msg} {
                            puts "+++ Breaking link for replica #$slave_id"
                        }
                    }
                }
            }
        }

        # 5) Increment the counter and wait for all the instances
        set x [$R($master_id) get x]
        test "PSYNC2: cluster is consistent after load (x = $x)" {
            for {set j 0} {$j < 5} {incr j} {
                wait_for_condition 50 1000 {
                    [$R($j) get x] == $counter_value
                } else {
                    fail "Instance #$j x variable is inconsistent"
                }
            }
        }

        # wait for all the slaves to be in sync with the master, due to pings, we have to re-sample the master constantly too
        wait_for_condition 500 100 {
            [status $R($master_id) master_repl_offset] == [status $R(0) master_repl_offset] &&
            [status $R($master_id) master_repl_offset] == [status $R(1) master_repl_offset] &&
            [status $R($master_id) master_repl_offset] == [status $R(2) master_repl_offset] &&
            [status $R($master_id) master_repl_offset] == [status $R(3) master_repl_offset] &&
            [status $R($master_id) master_repl_offset] == [status $R(4) master_repl_offset]
        } else {
            for {set j 0} {$j < 5} {incr j} {
                puts "$j: sync_full: [status $R($j) sync_full]"
                puts "$j: id1      : [status $R($j) master_replid]:[status $R($j) master_repl_offset]"
                puts "$j: id2      : [status $R($j) master_replid2]:[status $R($j) second_repl_offset]"
                puts "$j: backlog  : firstbyte=[status $R($j) repl_backlog_first_byte_offset] len=[status $R($j) repl_backlog_histlen]"
                puts "---"
            }
            fail "Slaves are not in sync with the master after too long time."
        }

        # Put down the old master so that it cannot generate more
        # replication stream, this way in the next master switch, the time at
        # which we move slaves away is not important, each will have full
        # history (otherwise PINGs will make certain slaves have more history),
        # and sometimes a full resync will be needed.
        $R($master_id) slaveof 127.0.0.1 0 ;# We use port zero to make it fail.

        if {$debug_msg} {
            for {set j 0} {$j < 5} {incr j} {
                puts "$j: sync_full: [status $R($j) sync_full]"
                puts "$j: id1      : [status $R($j) master_replid]:[status $R($j) master_repl_offset]"
                puts "$j: id2      : [status $R($j) master_replid2]:[status $R($j) second_repl_offset]"
                puts "$j: backlog  : firstbyte=[status $R($j) repl_backlog_first_byte_offset] len=[status $R($j) repl_backlog_histlen]"
                puts "---"
            }
        }

        test "PSYNC2: total sum of full synchronizations is exactly 4" {
            set sum 0
            for {set j 0} {$j < 5} {incr j} {
                incr sum [status $R($j) sync_full]
            }
            assert {$sum == 4}
        }

        # Limit anyway the maximum number of cycles. This is useful when the
        # test is skipped via --only option of the test suite. In that case
        # we don't want to see many seconds of this test being just skipped.
        if {$cycle > 50} break
    }

    test "PSYNC2: Bring the master back again for next test" {
        $R($master_id) slaveof no one
        set master_host $R_host($master_id)
        set master_port $R_port($master_id)
        for {set j 0} {$j < 5} {incr j} {
            if {$j == $master_id} continue
            $R($j) slaveof $master_host $master_port
        }

        # Wait for replicas to sync. it is not enough to just wait for connected_slaves==4
        # since we might do the check before the master realized that they're disconnected
        wait_for_condition 50 1000 {
            [status $R($master_id) connected_slaves] == 4 &&
            [status $R([expr {($master_id+1)%5}]) master_link_status] == "up" &&
            [status $R([expr {($master_id+2)%5}]) master_link_status] == "up" &&
            [status $R([expr {($master_id+3)%5}]) master_link_status] == "up" &&
            [status $R([expr {($master_id+4)%5}]) master_link_status] == "up"
        } else {
            fail "Replica not reconnecting"
        }
    }

    test "PSYNC2: Partial resync after restart using RDB aux fields" {
        # Pick a random slave
        set slave_id [expr {($master_id+1)%5}]
        set sync_count [status $R($master_id) sync_full]
        set sync_partial [status $R($master_id) sync_partial_ok]
        set sync_partial_err [status $R($master_id) sync_partial_err]
        catch {
            $R($slave_id) config rewrite
            $R($slave_id) debug restart
        }
        # note: just waiting for connected_slaves==4 has a race condition since
        # we might do the check before the master realized that the slave disconnected
        wait_for_condition 50 1000 {
            [status $R($master_id) sync_partial_ok] == $sync_partial + 1
        } else {
            puts "prev sync_full: $sync_count"
            puts "prev sync_partial_ok: $sync_partial"
            puts "prev sync_partial_err: $sync_partial_err"
            puts [$R($master_id) info stats]
            fail "Replica didn't partial sync"
        }
        set new_sync_count [status $R($master_id) sync_full]
        assert {$sync_count == $new_sync_count}
    }

    test "PSYNC2: Replica RDB restart with EVALSHA in backlog issue #4483" {
        # Pick a random slave
        set slave_id [expr {($master_id+1)%5}]
        set sync_count [status $R($master_id) sync_full]

        # Make sure to replicate the first EVAL while the salve is online
        # so that it's part of the scripts the master believes it's safe
        # to propagate as EVALSHA.
        $R($master_id) EVAL {return redis.call("incr","__mycounter")} 0
        $R($master_id) EVALSHA e6e0b547500efcec21eddb619ac3724081afee89 0

        # Wait for the two to sync
        wait_for_condition 50 1000 {
            [$R($master_id) debug digest] == [$R($slave_id) debug digest]
        } else {
            fail "Replica not reconnecting"
        }

        # Prevent the slave from receiving master updates, and at
        # the same time send a new script several times to the
        # master, so that we'll end with EVALSHA into the backlog.
        $R($slave_id) slaveof 127.0.0.1 0

        $R($master_id) EVALSHA e6e0b547500efcec21eddb619ac3724081afee89 0
        $R($master_id) EVALSHA e6e0b547500efcec21eddb619ac3724081afee89 0
        $R($master_id) EVALSHA e6e0b547500efcec21eddb619ac3724081afee89 0

        catch {
            $R($slave_id) config rewrite
            $R($slave_id) debug restart
        }

        # Reconfigure the slave correctly again, when it's back online.
        set retry 50
        while {$retry} {
            if {[catch {
                $R($slave_id) slaveof $master_host $master_port
            }]} {
                after 1000
            } else {
                break
            }
            incr retry -1
        }

        # The master should be back at 4 slaves eventually
        wait_for_condition 50 1000 {
            [status $R($master_id) connected_slaves] == 4
        } else {
            fail "Replica not reconnecting"
        }
        set new_sync_count [status $R($master_id) sync_full]
        assert {$sync_count == $new_sync_count}

        # However if the slave started with the full state of the
        # scripting engine, we should now have the same digest.
        wait_for_condition 50 1000 {
            [$R($master_id) debug digest] == [$R($slave_id) debug digest]
        } else {
            fail "Debug digest mismatch between master and replica in post-restart handshake"
        }
    }

    if {$no_exit} {
        while 1 { puts -nonewline .; flush stdout; after 1000}
    }

}}}}}

start_server {tags {"psync2"}} {
start_server {} {
start_server {} {
start_server {} {
start_server {} {
    test {pings at the end of replication stream are ignored for psync} {
        set master [srv -4 client]
        set master_host [srv -4 host]
        set master_port [srv -4 port]
        set replica1 [srv -3 client]
        set replica2 [srv -2 client]
        set replica3 [srv -1 client]
        set replica4 [srv -0 client]

        $replica1 replicaof $master_host $master_port
        $replica2 replicaof $master_host $master_port
        $replica3 replicaof $master_host $master_port
        $replica4 replicaof $master_host $master_port
        wait_for_condition 50 1000 {
            [status $master connected_slaves] == 4
        } else {
            fail "replicas didn't connect"
        }

        $master incr x
        wait_for_condition 50 1000 {
            [$replica1 get x] == 1 && [$replica2 get x] == 1 &&
            [$replica3 get x] == 1 && [$replica4 get x] == 1
        } else {
            fail "replicas didn't get incr"
        }

        # disconnect replica1 and replica2
        # and wait for the master to send a ping to replica3 and replica4
        $replica1 replicaof no one
        $replica2 replicaof 127.0.0.1 1 ;# we can't promote it to master since that will cycle the replication id
        $master config set repl-ping-replica-period 1
        after 1500

        # make everyone sync from the replica1 that didn't get the last ping from the old master
        # replica4 will keep syncing from the old master which now syncs from replica1
        # and replica2 will re-connect to the old master (which went back in time)
        set new_master_host [srv -3 host]
        set new_master_port [srv -3 port]
        $replica3 replicaof $new_master_host $new_master_port
        $master replicaof $new_master_host $new_master_port
        $replica2 replicaof $master_host $master_port
        wait_for_condition 50 1000 {
            [status $replica2 master_link_status] == "up" &&
            [status $replica3 master_link_status] == "up" &&
            [status $replica4 master_link_status] == "up" &&
            [status $master master_link_status] == "up"
        } else {
            fail "replicas didn't connect"
        }

        # make sure replication is still alive and kicking
        $replica1 incr x
        wait_for_condition 50 1000 {
            [$replica2 get x] == 2 &&
            [$replica3 get x] == 2 &&
            [$replica4 get x] == 2 &&
            [$master get x] == 2
        } else {
            fail "replicas didn't get incr"
        }

        # make sure there are full syncs other than the initial ones
        assert_equal [status $master sync_full] 4
        assert_equal [status $replica1 sync_full] 0
        assert_equal [status $replica2 sync_full] 0
        assert_equal [status $replica3 sync_full] 0
        assert_equal [status $replica4 sync_full] 0

        # force psync
        $master client kill type master
        $replica2 client kill type master
        $replica3 client kill type master
        $replica4 client kill type master

        # make sure replication is still alive and kicking
        $replica1 incr x
        wait_for_condition 50 1000 {
            [$replica2 get x] == 3 &&
            [$replica3 get x] == 3 &&
            [$replica4 get x] == 3 &&
            [$master get x] == 3
        } else {
            fail "replicas didn't get incr"
        }

        # make sure there are full syncs other than the initial ones
        assert_equal [status $master sync_full] 4
        assert_equal [status $replica1 sync_full] 0
        assert_equal [status $replica2 sync_full] 0
        assert_equal [status $replica3 sync_full] 0
        assert_equal [status $replica4 sync_full] 0
}
}}}}}
