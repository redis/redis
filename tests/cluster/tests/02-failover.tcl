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

test "Killing one master node" {
    kill_instance redis 0
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
