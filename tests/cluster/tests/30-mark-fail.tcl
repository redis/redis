# Check the basic monitoring and failover capabilities.

source "../tests/includes/init-tests.tcl"

test "Create a cluster with a single primary" {
    create_cluster 1 5
    set first_replica $::cluster_master_nodes
    puts "\nfirst replica: $first_replica"
}

test "Killing one replica" {
    set killed_replica_id [dict get [get_myself $first_replica] id]
    kill_instance redis $first_replica
}

test "Waiting for replica to be reported as failed" {
    wait_for_node_to_be_marked_fail $killed_replica_id
}

test "Restarting replica node" {
    restart_instance redis $first_replica
}
