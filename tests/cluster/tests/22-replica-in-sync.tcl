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

set master_id 0

test "Fill up" {
    R $master_id debug populate 10000000 key 100
}

test "Add new node as replica" {
    set replica_id [cluster_find_available_slave 1]
    set master_myself [get_myself $master_id]
    set replica_myself [get_myself $replica_id]
    set replica [dict get $replica_myself id]
    R $replica_id cluster replicate [dict get $master_myself id]
}

test "Check digest and replica state" {
    R 1 readonly
    wait_for_condition 1000 50 {
        [is_in_slots $master_id $replica]
    } else {
        fail "New replica didn't appear in the slots"
    }
    wait_for_condition 1000 50 {
        [is_replica_online [R $master_id info replication]]
    } else {
        fail "Replica is down for too long"
    }
    set replica_digest [R $replica_id debug digest]
    assert {$replica_digest ne 0}
}

test "Replica in loading state is hidden" {
    # Kill replica client for master and load new data to the primary
    R $master_id multi
    R $master_id config set repl-backlog-size 100
    R $master_id client kill type replica
    set num 10000
    set value [string repeat A 1024]
    for {set j 0} {$j < $num} {incr j} {
        set key "{0}"
        append key $j
        R $master_id set key $value
    }
    R $master_id exec

    # Check that replica started loading
    wait_for_condition 1000 50 {
        [s $replica_id loading] eq 1
    } else {
        fail "Replica didn't enter loading state"
    }
    # Check that replica is not in cluster slots
    assert {![is_in_slots $master_id $replica]}

    # Wait for sync to finish
    wait_for_condition 1000 50 {
        [s $replica_id loading] eq 0
    } else {
        fail "Replica is in loading state for too long"
    }

    # Check replica is back to cluster slots
    wait_for_condition 1000 50 {
        [is_in_slots $master_id $replica] 
    } else {
        fail "Replica is not back to slots"
    }
}

test "Check disconnected replica not hidden from slots" {
    # Disconnect replica from primary
    R $master_id client kill type replica
    # Check master to have no replicas
    assert {[s $master_id connected_slaves] == 0}
    # Check that replica is still in the cluster slots
    assert {[is_in_slots $master_id $replica]}
}
