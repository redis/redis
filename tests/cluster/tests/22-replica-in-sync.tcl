source "../tests/includes/init-tests.tcl"

test "Create a 1 node cluster" {
    create_cluster 1 0
}

test "Cluster is up" {
    assert_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test 0
}

proc is_in_slots {master_id replica} {
    set slots [R $master_id cluster slots]
    set found_position [string first $replica $slots]
    set result [expr {$found_position != -1}]
    return $result
}

proc is_replica_online {info_repl} {
    set found_position [string first "state=online" $info_repl]
    set result [expr {$found_position != -1}]
    return $result
}

proc get_last_pong_time {node_id target_cid} {
    foreach item [split [R $node_id cluster nodes] \n] {
        set args [split $item " "]
        if {[lindex $args 0] eq $target_cid} {
            return [lindex $args 5]
        }
    }
    fail "Target node ID was not present"
}

set master_id 0

test "Fill up primary with data" {
    # Set 1 MB of data
    R $master_id debug populate 1000 key 1000
}

test "Add new node as replica" {
    set replica_id 1
    set replica [R $replica_id CLUSTER MYID]
    R $replica_id cluster replicate [R $master_id CLUSTER MYID]
}

test "Check digest and replica state" {
    wait_for_condition 1000 50 {
        [is_in_slots $master_id $replica]
    } else {
        fail "New replica didn't appear in the slots"
    }

    wait_for_condition 100 50 {
        [is_replica_online [R $master_id info replication]]
    } else {
        fail "Replica is down for too long"
    }
    set replica_digest [R $replica_id debug digest]
    assert {$replica_digest ne 0}
}

test "Replica in loading state is hidden" {
    # Kill replica client for master and load new data to the primary
    R $master_id config set repl-backlog-size 100

    # Set the key load delay so that it will take at least
    # 2 seconds to fully load the data.
    R $replica_id config set key-load-delay 4000

    # Trigger event loop processing every 1024 bytes, this trigger
    # allows us to send and receive cluster messages, so we are setting
    # it low so that the cluster messages are sent more frequently.
    R $replica_id config set loading-process-events-interval-bytes 1024

    R $master_id multi
    R $master_id client kill type replica
    set num 100
    set value [string repeat A 1024]
    for {set j 0} {$j < $num} {incr j} {
        set key "{0}"
        append key $j
        R $master_id set $key $value
    }
    R $master_id exec

    # The master will be the last to know the replica
    # is loading, so we will wait on that and assert
    # the replica is loading afterwards. 
    wait_for_condition 100 50 {
        ![is_in_slots $master_id $replica]
    } else {
        fail "Replica was always present in cluster slots"
    }
    assert_equal 1 [s $replica_id loading]

    # Wait for the replica to finish full-sync and become online
    wait_for_condition 200 50 {
        [s $replica_id master_link_status] eq "up"
    } else {
        fail "Replica didn't finish loading"
    }

    # Return configs to default values
    R $replica_id config set loading-process-events-interval-bytes 2097152
    R $replica_id config set key-load-delay 0

    # Check replica is back in cluster slots
    wait_for_condition 100 50 {
        [is_in_slots $master_id $replica] 
    } else {
        fail "Replica is not back to slots"
    }
    assert_equal 1 [is_in_slots $replica_id $replica] 
}

test "Check disconnected replica not hidden from slots" {
    # We want to disconnect the replica, but keep it alive so it can still gossip

    # Make sure that the replica will not be able to re-connect to the master
    R $master_id config set requirepass asdf

    # Disconnect replica from primary
    R $master_id client kill type replica

    # Check master to have no replicas
    assert {[s $master_id connected_slaves] == 0}

    set replica_cid [R $replica_id cluster myid]
    set initial_pong [get_last_pong_time $master_id $replica_cid]
    wait_for_condition 50 100 {
        $initial_pong != [get_last_pong_time $master_id $replica_cid]
    } else {
        fail "Primary never received gossip from replica"
    }

    # Check that replica is still in the cluster slots
    assert {[is_in_slots $master_id $replica]}

    # undo config
    R $master_id config set requirepass ""
}
