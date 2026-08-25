# Manual takeover test

start_cluster 5 5 {tags {external:skip cluster valgrind:skip}} {

test "Cluster is up" {
    wait_for_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test [srv 0 port]
}

# For this test, disable replica failover until
# all of the primaries are confirmed killed. Otherwise
# there might be enough time to elect a replica.
set replica_ids { 5 6 7 }
foreach id $replica_ids {
    R $id config set cluster-replica-no-failover yes
}

test "Killing majority of master nodes" {
    cluster_kill_node 0
    cluster_kill_node 1
    cluster_kill_node 2
}

foreach id $replica_ids {
    R $id config set cluster-replica-no-failover no
}

test "Cluster should eventually be down" {
    wait_for_cluster_state fail {0 1 2}
}

test "Use takeover to bring slaves back" {
    foreach id $replica_ids {
        R $id cluster failover takeover
    }
}

test "Cluster should eventually be up again" {
    wait_for_cluster_state ok {0 1 2}
}

test "Cluster is writable" {
    cluster_write_test [srv -4 port]
}

test "Instance #5, #6, #7 are now masters" {
    assert {[s -5 role] eq {master}}
    assert {[s -6 role] eq {master}}
    assert {[s -7 role] eq {master}}
}

test "Restarting the previously killed master nodes" {
    cluster_restart_node 0
    cluster_restart_node 1
    cluster_restart_node 2
}

test "Instance #0, #1, #2 gets converted into a slaves" {
    wait_for_condition 1000 50 {
        [s 0 role] eq {slave} && [s -1 role] eq {slave} && [s -2 role] eq {slave}
    } else {
        fail "Old masters not converted into slaves"
    }
}

} ;# start_cluster
