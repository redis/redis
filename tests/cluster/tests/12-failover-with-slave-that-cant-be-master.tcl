# Check the can-be-elected-as-master configuration option
# and automatic failover.

source "../tests/includes/init-tests.tcl"

test "Create a 3 nodes cluster" {
    create_cluster 3 6
}

test "Cluster is up" {
    assert_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test 0
}

test "Instance #3 is a slave of instance #0" {
    assert {[RI 3 master_port] eq [get_instance_attrib redis 0 port]}
}

test "Instance #6 is a slave of instance #0" {
    assert {[RI 6 master_port] eq [get_instance_attrib redis 0 port]}
}

test "Instance #3 synced with the master" {
    wait_for_condition 1000 50 {
        [RI 3 master_link_status] eq {up}
    } else {
        fail "Instance #3 master link status is not up"
    }
}

test "Instance #6 synced with the master" {
    wait_for_condition 1000 50 {
        [RI 6 master_link_status] eq {up}
    } else {
        fail "Instance #6 master link status is not up"
    }
}

test "Marking instance #3 as \"can't be master\"" {
    cluster_cant_be_elected_as_master 3
}

set current_epoch [CI 3 cluster_current_epoch]

test "Killing one master node" {
    kill_instance redis 0
}

test "Wait for failover" {
    wait_for_condition 1000 50 {
        [CI 3 cluster_current_epoch] > $current_epoch
    } else {
        fail "No failover detected"
    }
}

test "Cluster should eventually be up again" {
    assert_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test 6
}

test "Instance #3 is a slave of instance #6" {
    assert {[RI 3 master_port] eq [get_instance_attrib redis 6 port]}
}

test "Instance #6 is now a master" {
    assert {[RI 6 role] eq {master}}
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

test "Instance #0 is a slave of instance #6" {
    assert {[RI 0 master_port] eq [get_instance_attrib redis 6 port]}
}
