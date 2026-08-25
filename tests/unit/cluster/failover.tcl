# Check the basic monitoring and failover capabilities.

start_cluster 5 5 {tags {external:skip cluster valgrind:skip}} {

test "Cluster is up" {
    wait_for_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test [srv 0 port]
}

test "Instance #5 is a slave" {
    assert {[s -5 role] eq {slave}}
}

test "Instance #5 synced with the master" {
    wait_for_condition 1000 50 {
        [s -5 master_link_status] eq {up}
    } else {
        fail "Instance #5 master link status is not up"
    }
}

set current_epoch [CI 1 cluster_current_epoch]

test "Killing one master node" {
    cluster_kill_node 0
}

test "Wait for failover" {
    wait_for_condition 1000 50 {
        [CI 1 cluster_current_epoch] > $current_epoch
    } else {
        fail "No failover detected"
    }
}

test "Cluster should eventually be up again" {
    wait_for_cluster_state ok {0}
}

test "Cluster is writable" {
    cluster_write_test [srv -1 port]
}

test "Instance #5 is now a master" {
    assert {[s -5 role] eq {master}}
}

test "Restarting the previously killed master node" {
    cluster_restart_node 0
}

test "Instance #0 gets converted into a slave" {
    wait_for_condition 1000 50 {
        [s 0 role] eq {slave}
    } else {
        fail "Old master was not converted into slave"
    }
}

} ;# start_cluster
