# Returns 1 if no node knows node_id, 0 if any node knows it.
proc node_is_forgotten {node_id} {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        set cluster_nodes [R $j CLUSTER NODES]
        if { [string match "*$node_id*" $cluster_nodes] } {
            return 0
        }
    }
    return 1
}

# Isolate a node from the cluster and give it a new nodeid
proc isolate_node {id} {
    set node_id [R $id CLUSTER MYID]
    R $id CLUSTER RESET HARD
    # Here we additionally test that CLUSTER FORGET propagates to all nodes.
    set other_id [expr $id == 0 ? 1 : 0]
    R $other_id CLUSTER FORGET $node_id
    wait_for_condition 50 100 {
        [node_is_forgotten $node_id]
    } else {
        fail "CLUSTER FORGET was not propagated to all nodes"
    }
}

# Check if cluster's view of human announced nodename is consistent
proc are_nodenames_propagated {match_string} {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        foreach n [get_cluster_nodes $j] {
            if { ![string match $match_string [dict get $n nodename]] } {
		    return 0
	    }
        }
    }
    return 1
}

proc get_node_info_from_shard {id reference {type node}} {
    set shards_response [R $reference CLUSTER SHARDS]
    foreach shard_response $shards_response {
        set nodes [dict get $shard_response nodes]
        foreach node $nodes {
            if {[dict get $node id] eq $id} {
                if {$type eq "node"} {
                    return $node
                } elseif {$type eq "shard"} {
                    return $shard_response
                } else {
                    return {}
                }
            }
        }
    }
    # No shard found, return nothing
    return {}
}

# Start a cluster with 3 masters and 4 replicas.
# These tests rely on specific node ordering, so make sure no node fails over.
start_cluster 3 4 {tags {external:skip cluster} overrides {cluster-replica-no-failover yes}} {
test "Set cluster human announced nodename and verify they are propagated" {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        R $j config set cluster-announce-human-nodename "nodename-$j.com"
    }
    wait_for_condition 50 100 {
        [are_nodenames_propagated "nodename-*.com"] eq 1
    } else {
        fail "cluster human announced nodename were not propagated"
    }

    # Now that everything is propagated, assert everyone agrees
    wait_for_cluster_propagation
}



test "Update human announced nodename and make sure they are all eventually propagated" {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        R $j config set cluster-announce-human-nodename "nodename-updated-$j.com"
    }

    wait_for_condition 50 100 {
        [are_nodenames_propagated "nodename-updated-*.com"] eq 1
    } else {
        fail "cluster human announced nodename were not propagated"
    }

    # Now that everything is propagated, assert everyone agrees
    wait_for_cluster_propagation
}


test "Remove human announced nodename and make sure they are all eventually propagated" {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        R $j config set cluster-announce-human-nodename ""
    }

    wait_for_condition 50 100 {
        [are_nodenames_propagated ""] eq 1
    } else {
        fail "cluster human announced nodename were not propagated"
    }

    # Now that everything is propagated, assert everyone agrees
    wait_for_cluster_propagation
}

test "Test restart will keep nodename information" {
    # Set a new nodename, reboot and make sure it sticks
    R 0 config set cluster-announce-human-nodename "restart-1.com"

    # Store the nodename in the config
    R 0 config rewrite

    restart_server 0 true false
    set node_0_id [R 0 CLUSTER MYID]
    wait_for_condition 50 100 {
        [string match "restart-1.com" [dict get [get_node_info_from_shard $node_0_id 4 "node"] "nodename"]] != 0
    } else {
        fail "cluster human announced nodename were not propagated after restart"
    }
    # As a sanity check, make sure everyone eventually agrees
    wait_for_cluster_propagation
}

test "Test human announced nodename validation" {
    catch {R 0 config set cluster-announce-human-nodename "?.com"} err
    assert_match "* may only contain alphanumeric characters, hyphens, dots or underscore*" $err

    # Note this isn't a valid nodename, but it passes our internal validation
    R 0 config set cluster-announce-human-nodename "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-."
}
}
