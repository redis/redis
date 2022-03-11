
proc show_cluster_status {} {
    uplevel 1 {
        # The following is the regexp we use to match the log line
        # time info. Logs are in the following form:
        #
        # 11296:M 25 May 2020 17:37:14.652 # Server initialized
        set log_regexp {^[0-9]+:[A-Z] [0-9]+ [A-z]+ [0-9]+ ([0-9:.]+) .*}
        set repl_regexp {(master|repl|sync|backlog|meaningful|offset)}

        puts "Master ID is $master_id"
        for {set j 0} {$j < 5} {incr j} {
            puts "$j: sync_full: [status $R($j) sync_full]"
            puts "$j: id1      : [status $R($j) master_replid]:[status $R($j) master_repl_offset]"
            puts "$j: id2      : [status $R($j) master_replid2]:[status $R($j) second_repl_offset]"
            puts "$j: backlog  : firstbyte=[status $R($j) repl_backlog_first_byte_offset] len=[status $R($j) repl_backlog_histlen]"
            puts "$j: x var is : [$R($j) GET x]"
            puts "---"
        }

        # Show the replication logs of every instance, interleaving
        # them by the log date.
        #
        # First: load the lines as lists for each instance.
        array set log {}
        for {set j 0} {$j < 5} {incr j} {
            set fd [open $R_log($j)]
            while {[gets $fd l] >= 0} {
                if {[regexp $log_regexp $l] &&
                    [regexp -nocase $repl_regexp $l]} {
                    lappend log($j) $l
                }
            }
            close $fd
        }

        # To interleave the lines, at every step consume the element of
        # the list with the lowest time and remove it. Do it until
        # all the lists are empty.
        #
        # regexp {^[0-9]+:[A-Z] [0-9]+ [A-z]+ [0-9]+ ([0-9:.]+) .*} $l - logdate
        while 1 {
            # Find the log with smallest time.
            set empty 0
            set best 0
            set bestdate {}
            for {set j 0} {$j < 5} {incr j} {
                if {[llength $log($j)] == 0} {
                    incr empty
                    continue
                }
                regexp $log_regexp [lindex $log($j) 0] - date
                if {$bestdate eq {}} {
                    set best $j
                    set bestdate $date
                } else {
                    if {[string compare $bestdate $date] > 0} {
                        set best $j
                        set bestdate $date
                    }
                }
            }
            if {$empty == 5} break ; # Our exit condition: no more logs

            # Emit the one with the smallest time (that is the first
            # event in the time line).
            puts "\[$best port $R_port($best)\] [lindex $log($best) 0]"
            set log($best) [lrange $log($best) 1 end]
        }
    }
}

start_server {tags {"psync2 external:skip"}} {
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

    set duration 40                 ; # Total test seconds

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
        set R_id_from_port($R_port($j)) $j ;# To get a replica index by port
        set R_log($j) [srv [expr 0-$j] stdout]
        if {$debug_msg} {puts "Log file: [srv [expr 0-$j] stdout]"}
    }

    set cycle 0
    while {([clock seconds]-$start_time) < $duration} {
        incr cycle
        test "PSYNC2: --- CYCLE $cycle ---" {}

        # Create a random replication layout.
        # Start with switching master (this simulates a failover).

        # 1) Select the new master.
        set master_id [randomInt 5]
        set used [list $master_id]
        test "PSYNC2: \[NEW LAYOUT\] Set #$master_id as master" {
            $R($master_id) slaveof no one
            $R($master_id) config set repl-ping-replica-period 1 ;# increase the chance that random ping will cause issues
            if {$counter_value == 0} {
                $R($master_id) set x $counter_value
            }
        }

        # Build a lookup with the root master of each replica (head of the chain).
        array set root_master {}
        for {set j 0} {$j < 5} {incr j} {
            set r $j
            while {1} {
                set r_master_port [status $R($r) master_port]
                if {$r_master_port == ""} {
                    set root_master($j) $r
                    break
                }
                set r_master_id $R_id_from_port($r_master_port)
                set r $r_master_id
            }
        }

        # Wait for the newly detached master-replica chain (new master and existing replicas that were
        # already connected to it, to get updated on the new replication id.
        # This is needed to avoid a race that can result in a full sync when a replica that already
        # got an updated repl id, tries to psync from one that's not yet aware of it.
        wait_for_condition 50 1000 {
            ([status $R(0) master_replid] == [status $R($root_master(0)) master_replid]) &&
            ([status $R(1) master_replid] == [status $R($root_master(1)) master_replid]) &&
            ([status $R(2) master_replid] == [status $R($root_master(2)) master_replid]) &&
            ([status $R(3) master_replid] == [status $R($root_master(3)) master_replid]) &&
            ([status $R(4) master_replid] == [status $R($root_master(4)) master_replid])
        } else {
            show_cluster_status
            fail "Replica did not inherit the new replid."
        }

        # Build a lookup with the direct connection master of each replica.
        # First loop that uses random to decide who replicates from who.
        array set slave_to_master {}
        while {[llength $used] != 5} {
            while 1 {
                set slave_id [randomInt 5]
                if {[lsearch -exact $used $slave_id] == -1} break
            }
            set rand [randomInt [llength $used]]
            set mid [lindex $used $rand]
            set slave_to_master($slave_id) $mid
            lappend used $slave_id
        }

        # 2) Attach all the slaves to a random instance
        # Second loop that does the actual SLAVEOF command and make sure execute it in the right order.
        while {[array size slave_to_master] > 0} {
            foreach slave_id [array names slave_to_master] {
                set mid $slave_to_master($slave_id)

                # We only attach the replica to a random instance that already in the old/new chain.
                if {$root_master($mid) == $root_master($master_id)} {
                    # Find a replica that can be attached to the new chain already attached to the new master.
                    # My new master is in the new chain.
                } elseif {$root_master($mid) == $root_master($slave_id)} {
                    # My new master and I are in the old chain.
                } else {
                    # In cycle 1, we do not care about the order.
                    if {$cycle != 1} {
                        # skipping this replica for now to avoid attaching in a bad order
                        # this is done to avoid an unexpected full sync, when we take a
                        # replica that already reconnected to the new chain and got a new replid
                        # and is then set to connect to a master that's still not aware of that new replid
                        continue
                    }
                }

                set master_host $R_host($master_id)
                set master_port $R_port($master_id)

                test "PSYNC2: Set #$slave_id to replicate from #$mid" {
                    $R($slave_id) slaveof $master_host $master_port
                }

                # Wait for replica to be connected before we proceed.
                wait_for_condition 50 1000 {
                    [status $R($slave_id) master_link_status] == "up"
                } else {
                    show_cluster_status
                    fail "Replica not reconnecting."
                }

                set root_master($slave_id) $root_master($mid)
                unset slave_to_master($slave_id)
                break
            }
        }

        # Wait for replicas to sync. so next loop won't get -LOADING error
        wait_for_condition 50 1000 {
            [status $R([expr {($master_id+1)%5}]) master_link_status] == "up" &&
            [status $R([expr {($master_id+2)%5}]) master_link_status] == "up" &&
            [status $R([expr {($master_id+3)%5}]) master_link_status] == "up" &&
            [status $R([expr {($master_id+4)%5}]) master_link_status] == "up"
        } else {
            show_cluster_status
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
                    show_cluster_status
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
                    show_cluster_status
                    fail "Instance #$j x variable is inconsistent"
                }
            }
        }

        # wait for all the slaves to be in sync.
        set masteroff [status $R($master_id) master_repl_offset]
        wait_for_condition 500 100 {
            [status $R(0) master_repl_offset] >= $masteroff &&
            [status $R(1) master_repl_offset] >= $masteroff &&
            [status $R(2) master_repl_offset] >= $masteroff &&
            [status $R(3) master_repl_offset] >= $masteroff &&
            [status $R(4) master_repl_offset] >= $masteroff
        } else {
            show_cluster_status
            fail "Replicas offsets didn't catch up with the master after too long time."
        }

        if {$debug_msg} {
            show_cluster_status
        }

        test "PSYNC2: total sum of full synchronizations is exactly 4" {
            set sum 0
            for {set j 0} {$j < 5} {incr j} {
                incr sum [status $R($j) sync_full]
            }
            if {$sum != 4} {
                show_cluster_status
                assert {$sum == 4}
            }
        }

        # In absence of pings, are the instances really able to have
        # the exact same offset?
        $R($master_id) config set repl-ping-replica-period 3600
        wait_for_condition 500 100 {
            [status $R($master_id) master_repl_offset] == [status $R(0) master_repl_offset] &&
            [status $R($master_id) master_repl_offset] == [status $R(1) master_repl_offset] &&
            [status $R($master_id) master_repl_offset] == [status $R(2) master_repl_offset] &&
            [status $R($master_id) master_repl_offset] == [status $R(3) master_repl_offset] &&
            [status $R($master_id) master_repl_offset] == [status $R(4) master_repl_offset]
        } else {
            show_cluster_status
            fail "Replicas and master offsets were unable to match *exactly*."
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
            show_cluster_status
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
            restart_server [expr {0-$slave_id}] true false
            set R($slave_id) [srv [expr {0-$slave_id}] client]
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
            show_cluster_status
            fail "Replica didn't partial sync"
        }
        set new_sync_count [status $R($master_id) sync_full]
        assert {$sync_count == $new_sync_count}
    }

    if {$no_exit} {
        while 1 { puts -nonewline .; flush stdout; after 1000}
    }

}}}}}
