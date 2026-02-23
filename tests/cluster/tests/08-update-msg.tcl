# Test UPDATE messages sent by other nodes when the currently authorirative
# master is unavailable. The test is performed in the following steps:
#
# 1) Master goes down.
# 2) Slave failover and becomes new master.
# 3) New master is partitioned away.
# 4) Old master returns.
# 5) At this point we expect the old master to turn into a slave ASAP because
#    of the UPDATE messages it will receive from the other nodes when its
#    configuration will be found to be outdated.

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

test "Killing the new master #5" {
    kill_instance redis 5
}

test "Cluster should be down now" {
    assert_cluster_state fail
}

test "Restarting the old master node" {
    restart_instance redis 0
}

test "Instance #0 gets converted into a slave" {
    wait_for_condition 1000 50 {
        [RI 0 role] eq {slave}
    } else {
        fail "Old master was not converted into slave"
    }
}

test "Restarting the new master node" {
    restart_instance redis 5
}

test "Cluster is up again" {
    assert_cluster_state ok
}
