# Helper functions specifically for setting up and configuring redis
# clusters.

# Check if cluster configuration is consistent.
proc cluster_config_consistent {} {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        if {$j == 0} {
            set base_cfg [R $j cluster slots]
        } else {
            if {[R $j cluster slots] != $base_cfg} {
                return 0
            }
        }
    }

    return 1
}

# Check if cluster size is consistent.
proc cluster_size_consistent {cluster_size} {
    for {set j 0} {$j < $cluster_size} {incr j} {
        if {[CI $j cluster_known_nodes] ne $cluster_size} {
            return 0
        }
    }
    return 1
}

# Wait for cluster configuration to propagate and be consistent across nodes.
proc wait_for_cluster_propagation {} {
    wait_for_condition 50 100 {
        [cluster_config_consistent] eq 1
    } else {
        fail "cluster config did not reach a consistent state"
    }
}

# Wait for cluster size to be consistent across nodes.
proc wait_for_cluster_size {cluster_size} {
    wait_for_condition 50 100 {
        [cluster_size_consistent $cluster_size] eq 1
    } else {
        fail "cluster size did not reach a consistent size $cluster_size"
    }
}

# Check that cluster nodes agree about "state", or raise an error.
proc wait_for_cluster_state {state} {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        wait_for_condition 100 50 {
            [CI $j cluster_state] eq $state
        } else {
            fail "Cluster node $j cluster_state:[CI $j cluster_state]"
        }
    }
}

# Default slot allocation for clusters, each master has a continuous block
# and approximately equal number of slots.
proc continuous_slot_allocation {masters} {
    set avg [expr double(16384) / $masters]
    set slot_start 0
    for {set j 0} {$j < $masters} {incr j} {
        set slot_end [expr int(ceil(($j + 1) * $avg) - 1)]
        R $j cluster addslotsrange $slot_start $slot_end
        set slot_start [expr $slot_end + 1]
    }
}

# Setup method to be executed to configure the cluster before the
# tests run.
proc cluster_setup {masters node_count slot_allocator code} {
    # Have all nodes meet
    for {set i 1} {$i < $node_count} {incr i} {
        R 0 CLUSTER MEET [srv -$i host] [srv -$i port]
    }

    $slot_allocator $masters

    wait_for_cluster_propagation

    # Setup master/replica relationships
    for {set i 0} {$i < $masters} {incr i} {
        set nodeid [R $i CLUSTER MYID]
        for {set j [expr $i + $masters]} {$j < $node_count} {incr j $masters} {
            R $j CLUSTER REPLICATE $nodeid
        }
    }

    wait_for_cluster_propagation
    wait_for_cluster_state "ok"

    uplevel 1 $code
}

# Start a cluster with the given number of masters and replicas. Replicas
# will be allocated to masters by round robin.
proc start_cluster {masters replicas options code {slot_allocator continuous_slot_allocation}} {
    set node_count [expr $masters + $replicas]

    # Set the final code to be the tests + cluster setup
    set code [list cluster_setup $masters $node_count $slot_allocator $code]

    # Configure the starting of multiple servers. Set cluster node timeout
    # aggressively since many tests depend on ping/pong messages. 
    set cluster_options [list overrides [list cluster-enabled yes cluster-node-timeout 500]]
    set options [concat $cluster_options $options]

    # Cluster mode only supports a single database, so before executing the tests
    # it needs to be configured correctly and needs to be reset after the tests. 
    set old_singledb $::singledb
    set ::singledb 1
    start_multiple_servers $node_count $options $code
    set ::singledb $old_singledb
}
