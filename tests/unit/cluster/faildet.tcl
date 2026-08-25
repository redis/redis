# Check the basic monitoring and failover capabilities.

start_cluster 5 5 {tags {external:skip cluster valgrind:skip}} {

test "Cluster should start ok" {
    wait_for_cluster_state ok
}

test "Killing two slave nodes" {
    cluster_kill_node 5
    cluster_kill_node 6
}

test "Cluster should be still up" {
    wait_for_cluster_state ok {5 6}
}

test "Killing one master node" {
    cluster_kill_node 0
}

# Note: the only slave of instance 0 is already down so no
# failover is possible, that would change the state back to ok.
test "Cluster should be down now" {
    wait_for_cluster_state fail {0 5 6}
}

test "Restarting master node" {
    cluster_restart_node 0
}

test "Cluster should be up again" {
    wait_for_cluster_state ok {5 6}
}

} ;# start_cluster
