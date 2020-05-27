# Test the meaningful offset implementation to make sure masters
# are able to PSYNC with replicas even if the replication stream
# has pending PINGs at the end.

start_server {tags {"psync2"}} {
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
    test "PSYNC2 meaningful offset: setup" {
        $R(1) replicaof $R_host(0) $R_port(0)
        $R(0) set foo bar
        wait_for_condition 50 1000 {
            [status $R(1) master_link_status] == "up" &&
            [$R(0) dbsize] == 1 && [$R(1) dbsize] == 1
        } else {
            fail "Replicas not replicating from master"
        }
    }

    test "PSYNC2 meaningful offset: write and wait replication" {
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
    test "PSYNC2 meaningful offset: pause replica and promote it" {
        $R(1) MULTI
        $R(1) DEBUG SLEEP 5
        $R(1) SLAVEOF NO ONE
        $R(1) EXEC
        $R(1) ping ; # Wait for it to return back available
    }

    test "Make the old master a replica of the new one and check conditions" {
        set sync_partial [status $R(1) sync_partial_ok]
        assert {$sync_partial == 0}
        $R(0) REPLICAOF $R_host(1) $R_port(1)
        wait_for_condition 50 1000 {
            [status $R(1) sync_partial_ok] == 1
        } else {
            fail "The new master was not able to partial sync"
        }
    }
}}
