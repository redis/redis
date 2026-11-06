# These tests were added together with the meaningful offset implementation
# in redis 6.0.0, which was later abandoned in 6.0.4, they used to test that
# servers are able to PSYNC with replicas even if the replication stream has
# PINGs at the end which present in one sever and missing on another.
# We keep these tests just because they reproduce edge cases in the replication
# logic in hope they'll be able to spot some problem in the future.

start_server {tags {"psync2 external:skip"}} {
start_server {} {
    # Config
    set debug_msg 0                 ; # Enable additional debug messages

    for {set j 0} {$j < 2} {incr j} {
        set R($j) [srv [expr 0-$j] client]
        set R_host($j) [srv [expr 0-$j] host]
        set R_port($j) [srv [expr 0-$j] port]
        $R($j) CONFIG SET repl-ping-replica-period 1
        if {$debug_msg} {puts "Log file: [srv [expr 0-$j] stdout]"}
    }

    # Setup replication
    test "PSYNC2 pingoff: setup" {
        $R(1) replicaof $R_host(0) $R_port(0)
        $R(0) set foo bar
        wait_for_condition 50 1000 {
            [status $R(1) master_link_status] == "up" &&
            [$R(0) dbsize] == 1 && [$R(1) dbsize] == 1
        } else {
            fail "Replicas not replicating from master"
        }
    }

    test "PSYNC2 pingoff: write and wait replication" {
        $R(0) INCR counter
        $R(0) INCR counter
        $R(0) INCR counter
        wait_for_condition 50 1000 {
            [$R(0) GET counter] eq [$R(1) GET counter]
        } else {
            fail "Master and replica don't agree about counter"
        }
    }

    # In this test we'll make sure the replica will get stuck, but with
    # an active connection: this way the master will continue to send PINGs
    # every second (we modified the PING period earlier)
    test "PSYNC2 pingoff: pause replica and promote it" {
        $R(1) MULTI
        $R(1) DEBUG SLEEP 5
        $R(1) SLAVEOF NO ONE
        $R(1) EXEC
        $R(1) ping ; # Wait for it to return back available
    }

    test "Make the old master a replica of the new one and check conditions" {
        # We set the new master's ping period to a high value, so that there's
        # no chance for a race condition of sending a PING in between the two
        # INFO calls in the assert for master_repl_offset match below.
        $R(1) CONFIG SET repl-ping-replica-period 1000

        assert_equal [status $R(1) sync_full] 0
        $R(0) REPLICAOF $R_host(1) $R_port(1)

        wait_for_condition 50 1000 {
            [status $R(0) master_link_status] == "up"
        } else {
            fail "The new master was not able to sync"
        }

        # make sure replication is still alive and kicking
        $R(1) incr x
        wait_for_condition 50 1000 {
            [status $R(0) loading] == 0 &&
            [$R(0) get x] == 1
        } else {
            fail "replica didn't get incr"
        }
        assert_equal [status $R(0) master_repl_offset] [status $R(1) master_repl_offset]
    }
}}


start_server {tags {"psync2 external:skip"}} {
start_server {} {
start_server {} {
start_server {} {
start_server {} {
    test {test various edge cases of repl topology changes with missing pings at the end} {
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
        set replofs [status $master master_repl_offset]
        wait_for_condition 50 100 {
            [status $replica3 master_repl_offset] > $replofs &&
            [status $replica4 master_repl_offset] > $replofs
        } else {
            fail "replica didn't sync in time"
        }

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

        # make sure we have the right amount of full syncs
        assert_equal [status $master sync_full] 6
        assert_equal [status $replica1 sync_full] 2
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

        # make sure we have the right amount of full syncs
        assert_equal [status $master sync_full] 6
        assert_equal [status $replica1 sync_full] 2
        assert_equal [status $replica2 sync_full] 0
        assert_equal [status $replica3 sync_full] 0
        assert_equal [status $replica4 sync_full] 0
}
}}}}}

start_server {tags {"psync2 external:skip"}} {
start_server {} {
start_server {} {

    for {set j 0} {$j < 3} {incr j} {
        set R($j) [srv [expr 0-$j] client]
        set R_host($j) [srv [expr 0-$j] host]
        set R_port($j) [srv [expr 0-$j] port]
        $R($j) CONFIG SET repl-ping-replica-period 1
    }

    test "Chained replicas disconnect when replica re-connect with the same master" {
        # Add a second replica as a chained replica of the current replica
        $R(1) replicaof $R_host(0) $R_port(0)
        $R(2) replicaof $R_host(1) $R_port(1)
        wait_for_condition 50 1000 {
            [status $R(2) master_link_status] == "up"
        } else {
            fail "Chained replica not replicating from its master"
        }

        # Do a write on the master, and wait for the master to
        # send some PINGs to its replica
        $R(0) INCR counter2
        set replofs [status $R(0) master_repl_offset]
        wait_for_condition 50 100 {
            [status $R(1) master_repl_offset] > $replofs &&
            [status $R(2) master_repl_offset] > $replofs
        } else {
            fail "replica didn't sync in time"
        }
        set sync_partial_master [status $R(0) sync_partial_ok]
        set sync_partial_replica [status $R(1) sync_partial_ok]
        $R(0) CONFIG SET repl-ping-replica-period 100

        # Disconnect the master's direct replica
        $R(0) client kill type replica
        wait_for_condition 50 1000 {
            [status $R(1) master_link_status] == "up" && 
            [status $R(2) master_link_status] == "up" &&
            [status $R(0) sync_partial_ok] == $sync_partial_master + 1 &&
            [status $R(1) sync_partial_ok] == $sync_partial_replica
        } else {
            fail "Disconnected replica failed to PSYNC with master"
        }

        # Verify that the replica and its replica's meaningful and real
        # offsets match with the master
        assert_equal [status $R(0) master_repl_offset] [status $R(1) master_repl_offset]
        assert_equal [status $R(0) master_repl_offset] [status $R(2) master_repl_offset]

        # make sure replication is still alive and kicking
        $R(0) incr counter2
        wait_for_condition 50 1000 {
            [$R(1) get counter2] == 2 && [$R(2) get counter2] == 2
        } else {
            fail "replicas didn't get incr"
        }
        assert_equal [status $R(0) master_repl_offset] [status $R(1) master_repl_offset]
        assert_equal [status $R(0) master_repl_offset] [status $R(2) master_repl_offset]
    }
}}}
