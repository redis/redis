# Check the basic monitoring and failover capabilities.

source "../tests/includes/init-tests.tcl"

test "Create a 5 nodes cluster" {
    create_cluster 5 5
}

test "Killing one master node" {
    kill_instance redis 0
}

test "Cluster should be down now" {
    assert_cluster_state fail
}

test "Restarting master node" {
    restart_instance redis 0
}

test "Cluster should be up again" {
    assert_cluster_state ok
}

test "Killing two slave nodes" {
    kill_instance redis 5
    kill_instance redis 6
}

test "Cluster should be still up" {
    assert_cluster_state ok
}
