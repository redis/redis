# Check the basic monitoring and failover capabilities.

source "../tests/includes/init-tests.tcl"

proc create_cluster {masters slaves} {
    cluster_allocate_slots $masters
    if {$slaves} {
        cluster_allocate_slaves $masters $slaves
    }
    assert_cluster_state ok
}

test "Create a 5 nodes cluster" {
    create_cluster 5 0
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
