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

set numkeys 10000000000
set numops 100000
array set content {}
set master_id 0

proc is_in_slots {id slots} {
    set found_position [string first $id $slots]
    set result [expr {$found_position != -1}]
    return $result
}

test "Fill up with keys" {
    for {set j 0} {$j < $numops} {incr j} {
        set listid [randomInt $numkeys]
        set key "key:$listid"
        set ele [randomValue]
        R 0 rpush $key $ele
        lappend content($key) $ele
    }
}

test "Add new node as replica" {
    set replica_id [cluster_find_available_slave 1]
    set master_myself [get_myself $master_id]
    R $replica_id clusteradmin replicate [dict get $master_myself id]
}

test "Check keys exist in the new replica" {
    R 1 readonly
    while (1) {
        set slots [R 0 cluster slots]
        set replica_myself [get_myself $replica_id]
        set replica [dict get $replica_myself id]
        if ([is_in_slots $replica $slots]) {
            foreach {key value} [array get content] {
                set is_exist [R 1 exists $key]
                assert {$is_exist}
            }
            break
        }
    }
}