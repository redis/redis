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

proc is_in_slots {id slots} {
    set found_position [string first $id $slots]
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
    R $master_id debug populate 1000000 key 100
}

test "Add new node as replica" {
    set replica_id [cluster_find_available_slave 1]
    set master_myself [get_myself $master_id]
    set replica_myself [get_myself $replica_id]
    set replica [dict get $replica_myself id]
    R $replica_id clusteradmin replicate [dict get $master_myself id]
}

test "Check digest and replica state" {
    R 1 readonly
    while (1) {
        set slots [R $master_id cluster slots]
        if ([is_in_slots $replica $slots]) {
            set master_digest [R $master_id debug digest]
            set replica_digest [R $replica_id debug digest]
            set info_repl [R $master_id info replication]
            assert {$master_digest eq $replica_digest}
            assert {[is_replica_online $info_repl]}
            break
        }
    }
}