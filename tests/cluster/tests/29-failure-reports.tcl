# Test for issue #4256: crash in clusterNodeAddFailureReport
# The crash occurred in listNext() when iterating over fail_reports
# with a corrupted list/node pointer. This test stresses the failure
# report paths by repeatedly killing and restarting nodes.
source ../tests/includes/init-tests.tcl

test "Create a 5 nodes cluster" {
    create_cluster 3 3
}

test "Cluster should start ok" {
    assert_cluster_state ok
}

set num_cycles 5
set current_epoch [CI 1 cluster_current_epoch]

test "Stress failure reports by cycling nodes up and down" {
    for {set cycle 0} {$cycle < $num_cycles} {incr cycle} {
        # Kill a master node to trigger PFAIL/FAIL detection
        set killed_node [expr {$cycle % 3}]
        kill_instance redis $killed_node

        # Wait for failure reports to propagate via gossip
        after 2000
        # Force gossip exchange via CLUSTER BUMPEPOCH on remaining nodes
        for {set j 0} {$j < 3} {incr j} {
            if {$j != $killed_node} {
                catch {R $j CLUSTER BUMPEPOCH}
            }
        }

        # Restart the killed node
        restart_instance redis $killed_node

        # Wait for cluster to stabilize
        after 1000

        # Verify nodes are reachable
        for {set j 0} {$j < 3} {incr j} {
            wait_for_condition 500 100 {
                [catch {R $j PING} reply] == 0 && $reply eq {PONG}
            } else {
                fail "Node #$j not reachable after cycle $cycle"
            }
        }
    }
}

test "Cluster should eventually be up" {
    assert_cluster_state ok
}
