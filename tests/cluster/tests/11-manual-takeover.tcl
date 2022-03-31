# Manual takeover test

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

# For this test, disable replica failover until
# all of the primaries are confirmed killed. Otherwise
# there might be enough time to elect a replica.
set replica_ids { 5 6 7 }
foreach id $replica_ids {
    R $id config set cluster-replica-no-failover yes
}

test "Killing majority of master nodes" {
    kill_instance redis 0
    kill_instance redis 1
    kill_instance redis 2
}

foreach id $replica_ids {
    R $id config set cluster-replica-no-failover no
}

test "Cluster should eventually be down" {
    assert_cluster_state fail
}

test "Use takeover to bring slaves back" {
    foreach id $replica_ids {
        R $id cluster failover takeover
    }
}

test "Cluster should eventually be up again" {
    assert_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test 4
}

test "Instance #5, #6, #7 are now masters" {
    foreach id $replica_ids {
        assert {[RI $id role] eq {master}}
    }
}

test "Restarting the previously killed master nodes" {
    restart_instance redis 0
    restart_instance redis 1
    restart_instance redis 2
}

test "Instance #0, #1, #2 gets converted into a slaves" {
    wait_for_condition 1000 50 {
        [RI 0 role] eq {slave} && [RI 1 role] eq {slave} && [RI 2 role] eq {slave}
    } else {
        fail "Old masters not converted into slaves"
    }
}
