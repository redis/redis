# Tests for the RedisModuleEvent_ClusterTopologyChange server event. A test
# module (cluster_topology.so) subscribes to the event and counts the subevents;
# here we drive topology changes and assert that modules get notified.

set testmodule [file normalize tests/modules/cluster_topology.so]

# Returns the cluster_topology.stats of a node as a dict with the fields:
#   events slot role state node
# where 'slot'/'role'/'state'/'node' count the notifications whose change_flags
# bitmask had the SLOT / ROLE / STATE / NODE reason set.
proc topo_stats {node} {
    R $node cluster_topology.stats
}

start_cluster 3 3 [list tags {external:skip cluster modules} config_lines [list loadmodule $testmodule cluster-node-timeout 3000]] {
    test "ClusterTopologyChange notifies modules once the cluster becomes ready" {
        # cluster_setup has already waited for the cluster to be OK on every node.
        for {set i 0} {$i < 6} {incr i} {
            assert {[dict get [topo_stats $i] state] >= 1}
        }
    }

    test "ClusterTopologyChange reports the NODE reason as nodes are discovered" {
        # Bringing the cluster up adds every node to each node's view, so the
        # NODE reason must have been reported at least once on startup.
        for {set i 0} {$i < 6} {incr i} {
            assert {[dict get [topo_stats $i] node] >= 1}
        }
    }

    test "ClusterTopologyChange reports the SLOT reason on slot ownership change" {
        set before [dict get [topo_stats 0] slot]
        R 0 cluster DELSLOTSRANGE 0 100
        wait_for_condition 50 100 {
            [dict get [topo_stats 0] slot] > $before
        } else {
            fail "SLOT change reason was not reported after removing slots"
        }
        # Restore the slots so the cluster is whole again for the next test.
        R 0 cluster ADDSLOTSRANGE 0 100
        wait_for_cluster_state ok
    }

    test "ClusterTopologyChange reports the ROLE reason on failover" {
        # Find a replica and trigger a coordinated manual failover.
        set replica -1
        for {set i 0} {$i < 6} {incr i} {
            if {[lindex [R $i role] 0] eq "slave"} { set replica $i; break }
        }
        assert {$replica != -1}

        set before [dict get [topo_stats $replica] role]
        R $replica cluster failover
        # Wait for the replica to actually take over as a primary.
        wait_for_condition 50 100 {
            [lindex [R $replica role] 0] eq "master"
        } else {
            fail "manual failover did not promote the replica"
        }
        # The promotion must have notified the module with the ROLE reason.
        wait_for_condition 50 100 {
            [dict get [topo_stats $replica] role] > $before
        } else {
            fail "ROLE change reason was not reported on the promoted node"
        }
    }
}
