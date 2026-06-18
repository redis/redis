# Tests for the RedisModuleEvent_ClusterTopologyChange server event. A test
# module (cluster_topology.so) subscribes to the event and counts the subevents;
# here we drive topology changes and assert that modules get notified.

set testmodule [file normalize tests/modules/cluster_topology.so]

# Returns the cluster_topology.stats list of a node:
#   {startup topology primary other}
proc topo_stats {node} {
    R $node cluster_topology.stats
}

start_cluster 3 3 [list tags {external:skip cluster modules} config_lines [list loadmodule $testmodule cluster-node-timeout 3000]] {
    test "ClusterTopologyChange STARTUP fires once the cluster becomes ready" {
        # cluster_setup has already waited for the cluster to be OK on every node.
        for {set i 0} {$i < 6} {incr i} {
            assert {[lindex [topo_stats $i] 0] >= 1}
        }
    }

    test "ClusterTopologyChange TOPOLOGY_CHANGED fires on slot ownership change" {
        set before [lindex [topo_stats 0] 1]
        R 0 cluster DELSLOTSRANGE 0 100
        wait_for_condition 50 100 {
            [lindex [topo_stats 0] 1] > $before
        } else {
            fail "TOPOLOGY_CHANGED was not fired after removing slots"
        }
        # Restore the slots so the cluster is whole again for the next test.
        R 0 cluster ADDSLOTSRANGE 0 100
        wait_for_cluster_state ok
    }

    test "ClusterTopologyChange ROLE_CHANGED fires on failover" {
        # Find a replica and trigger a coordinated manual failover.
        set replica -1
        for {set i 0} {$i < 6} {incr i} {
            if {[lindex [R $i role] 0] eq "slave"} { set replica $i; break }
        }
        assert {$replica != -1}

        set before [lindex [topo_stats $replica] 2]
        R $replica cluster failover
        # Wait for the replica to actually take over as a primary.
        wait_for_condition 50 100 {
            [lindex [R $replica role] 0] eq "master"
        } else {
            fail "manual failover did not promote the replica"
        }
        # The promotion must have notified the module via ROLE_CHANGED.
        wait_for_condition 50 100 {
            [lindex [topo_stats $replica] 2] > $before
        } else {
            fail "ROLE_CHANGED was not fired on the promoted node"
        }
    }
}
