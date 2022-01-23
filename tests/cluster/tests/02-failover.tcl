# Check the basic monitoring and failover capabilities.

source "../tests/includes/init-tests.tcl"

test "Create a 5 nodes cluster" {
    create_cluster 5 5
}

test "Cluster is up" {
    assert_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test 0
}

test "Instance #5 is a slave" {
    assert {[RI 5 role] eq {slave}}
}

test "Instance #5 synced with the master" {
    wait_for_condition 1000 50 {
        [RI 5 master_link_status] eq {up}
    } else {
        fail "Instance #5 master link status is not up"
    }
}

set current_epoch [CI 1 cluster_current_epoch]
set instance_0_id  [dict get [get_myself 0] id]
set instance_0_slots [dict get [get_myself 0] slots]

test "Killing one master node" {
    kill_instance redis 0
}

test "Wait for failover" {
    wait_for_condition 1000 50 {
        [CI 1 cluster_current_epoch] > $current_epoch
    } else {
        fail "No failover detected"
    }
}

test "Cluster should eventually be up again" {
    assert_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test 1
}

test "Instance #5 is now a master" {
    assert {[RI 5 role] eq {master}}
}

test "Instance #0 failed but retains its previous slot_info" {
    foreach_redis_id id {
        if {$id == 0} continue
        set instance_0_node [get_node_by_id $id $instance_0_id] 
        assert {[lsearch -exact [dict get $instance_0_node flags] "fail"] != -1}
        assert {[dict get $instance_0_node slots] eq $instance_0_slots}
    }
}

test "Instance #6 does not forget instance #0's slot info after restart" {
    set id 6
    kill_instance redis $id
    restart_instance redis $id
    set instance_0_node [get_node_by_id $id $instance_0_id]
    assert {[lsearch -exact [dict get $instance_0_node flags] "fail"] != -1}
    assert {[dict get $instance_0_node slots] eq $instance_0_slots}
}

test "Restarting the previously killed master node" {
    restart_instance redis 0
}

test "Instance #0 gets converted into a slave" {
    wait_for_condition 1000 50 {
        [RI 0 role] eq {slave}
    } else {
        fail "Old master was not converted into slave"
    }
}

test "Instance #0 has no slot info associated after becoming a slave" {
    foreach_redis_id id {
        wait_for_condition 1000 50 {
            [dict get [get_node_by_id $id $instance_0_id] slots] eq ""
        } else {
            fail "Instance #0 has slot info associated with it while being slave."
        }
    }
}