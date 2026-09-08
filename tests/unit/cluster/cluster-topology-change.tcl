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

# Returns whether node_id's persisted cluster entry contains the given port.
# Persisting the config lets tests verify that an announced TCP/TLS port was
# learned even when it is not the cluster's preferred client port.
proc cluster_config_has_port {node node_id field port} {
    R $node cluster saveconfig
    set dir [lindex [R $node config get dir] 1]
    set filename [lindex [R $node config get cluster-config-file] 1]
    set fd [open [file join $dir $filename] r]
    set contents [read $fd]
    close $fd

    foreach line [split $contents "\n"] {
        if {[string match "$node_id *" $line]} {
            # nodes.conf keeps TCP in the main address field for backward
            # compatibility and stores TLS as an auxiliary field.
            if {$field eq "tcp-port"} {
                return [string match "$node_id *:$port@*" $line]
            }
            return [expr {[string first ",$field=$port" $line] != -1}]
        }
    }
    return 0
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

    test "ClusterTopologyChange reports the NODE reason on a node address change" {
        # Changing node 0's announced port makes the other nodes observe a new
        # address for node 0 (learned via gossip), which is a topology change.
        if {$::tls} {
            set baseport [lindex [R 0 config get tls-port] 1]
        } else {
            set baseport [lindex [R 0 config get port] 1]
        }
        set newport [find_available_port $baseport [expr [llength $::servers] + 1]]

        set before [dict get [topo_stats 1] node]
        R 0 config set cluster-announce-tls-port $newport
        R 0 config set cluster-announce-port $newport

        # Node 1 must both learn the new address and be notified with NODE.
        wait_for_condition 50 100 {
            [string match "*:$newport@*" [R 1 cluster nodes]] &&
            [dict get [topo_stats 1] node] > $before
        } else {
            fail "NODE change reason was not reported on a node address change"
        }

        # Restore node 0's announced port for the following tests.
        R 0 config set cluster-announce-tls-port 0
        R 0 config set cluster-announce-port 0
        wait_for_cluster_state ok
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

    test "ClusterTopologyChange reports the SLOT reason on atomic slot migration" {
        # An atomic migration reassigns slot ownership cluster-wide, so every node
        # (not just the losing and gaining owners) must observe the SLOT reason.
        set before {}
        for {set i 0} {$i < 6} {incr i} {
            dict set before $i [dict get [topo_stats $i] slot]
        }
        R 1 cluster migration import 0 100
        for {set i 0} {$i < 6} {incr i} {
            wait_for_condition 50 100 {
                [dict get [topo_stats $i] slot] > [dict get $before $i]
            } else {
                fail "SLOT change reason was not reported on node $i after atomic slot migration"
            }
        }
        # Migrate the slots back so the cluster layout is restored.
        wait_for_asm_done
        R 0 cluster migration import 0 100
        wait_for_asm_done
        wait_for_cluster_state ok
    }

    test "ClusterTopologyChange reports the STATE reason when the cluster turns FAIL" {
        set before [dict get [topo_stats 0] state]
        # Dropping ownership of slots leaves them uncovered, which (with the
        # default cluster-require-full-coverage) turns the cluster state to FAIL.
        R 0 cluster DELSLOTSRANGE 0 100
        wait_for_condition 50 100 {
            [CI 0 cluster_state] eq "fail"
        } else {
            fail "cluster did not turn FAIL after dropping slots"
        }
        wait_for_condition 50 100 {
            [dict get [topo_stats 0] state] > $before
        } else {
            fail "STATE change reason was not reported when the cluster turned FAIL"
        }
        # Restore the slots and let the cluster become OK again.
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

        # Snapshot the ROLE counters on every node before the failover.
        set before {}
        for {set i 0} {$i < 6} {incr i} {
            dict set before $i [dict get [topo_stats $i] role]
        }
        R $replica cluster failover
        # Wait for the replica to actually take over as a primary.
        wait_for_condition 50 100 {
            [lindex [R $replica role] 0] eq "master"
        } else {
            fail "manual failover did not promote the replica"
        }
        # The role change propagates across the cluster, so every node must be
        # notified with the ROLE reason, not just the promoted node.
        for {set i 0} {$i < 6} {incr i} {
            wait_for_condition 50 100 {
                [dict get [topo_stats $i] role] > [dict get $before $i]
            } else {
                fail "ROLE change reason was not reported on node $i"
            }
        }
    }

    # NOTE: keep this test last. It removes a node from the cluster, so it must
    # run after the failover test (which asserts that *every* node is notified).
    test "ClusterTopologyChange reports the NODE reason when a node is removed" {
        # Remove a replica (dropping a replica keeps every slot covered).
        set removed -1
        for {set i 1} {$i < 6} {incr i} {
            if {[lindex [R $i role] 0] eq "slave"} { set removed $i; break }
        }
        assert {$removed != -1}

        # Snapshot the NODE counters on every surviving node.
        set before {}
        for {set i 0} {$i < 6} {incr i} {
            if {$i == $removed} continue
            dict set before $i [dict get [topo_stats $i] node]
        }

        # isolate_node resets the node and forgets it; the FORGET propagates so
        # every surviving node eventually drops it from its view.
        isolate_node $removed

        # The removal must have notified the module with the NODE reason on each
        # surviving node, not just the one that issued the FORGET.
        for {set i 0} {$i < 6} {incr i} {
            if {$i == $removed} continue
            wait_for_condition 50 100 {
                [dict get [topo_stats $i] node] > [dict get $before $i]
            } else {
                fail "NODE change reason was not reported on node $i"
            }
        }
    }
}

start_cluster 2 2 [list tags {external:skip cluster modules} config_lines [list loadmodule $testmodule cluster-node-timeout 3000]] {
    test "ClusterTopologyChange does not report NODE when only a peer cluster bus port changes" {
        if {$::tls} {
            set baseport [lindex [R 0 config get tls-port] 1]
        } else {
            set baseport [lindex [R 0 config get port] 1]
        }
        set count [expr [llength $::servers] + 1]
        set newbus [find_available_port $baseport $count]

        set before [dict get [topo_stats 1] node]
        R 0 config set cluster-announce-bus-port $newbus

        wait_for_condition 50 100 {
            [string match "*@$newbus *" [R 1 cluster nodes]]
        } else {
            fail "Cluster announced bus port was not propagated via gossip"
        }
        assert_equal $before [dict get [topo_stats 1] node]
    }

    test "ClusterTopologyChange reports NODE when peer client ports change" {
        set node_id [R 0 cluster myid]
        foreach change {
            {cluster-announce-port tcp-port port 32004}
            {cluster-announce-tls-port tls-port tls-port 32005}
        } {
            lassign $change config field default_config newport
            set default_port [lindex [R 0 config get $default_config] 1]

            set local_before [dict get [topo_stats 0] node]
            set peer_before [dict get [topo_stats 1] node]
            R 0 config set $config $newport

            wait_for_condition 50 100 {
                [cluster_config_has_port 1 $node_id $field $newport] &&
                [dict get [topo_stats 1] node] > $peer_before
            } else {
                fail "NODE change reason was not reported after changing peer $config"
            }
            assert {[dict get [topo_stats 0] node] > $local_before}

            set local_before [dict get [topo_stats 0] node]
            set peer_before [dict get [topo_stats 1] node]
            R 0 config set $config 0

            wait_for_condition 50 100 {
                [cluster_config_has_port 1 $node_id $field $default_port] &&
                [dict get [topo_stats 1] node] > $peer_before
            } else {
                fail "NODE change reason was not reported after resetting peer $config"
            }
            assert {[dict get [topo_stats 0] node] > $local_before}
        }
    }
}

start_cluster 1 0 [list tags {external:skip cluster modules} config_lines [list loadmodule $testmodule]] {
    test "ClusterTopologyChange reports the NODE reason when local announced endpoints change" {
        foreach change {
            {cluster-announce-ip 127.0.0.2 ""}
            {cluster-announce-hostname node.example.test ""}
            {cluster-announce-port 32001 0}
            {cluster-announce-tls-port 32002 0}
        } {
            lassign $change config value default

            set before [dict get [topo_stats 0] node]
            R 0 config set $config $value
            wait_for_condition 50 100 {
                [dict get [topo_stats 0] node] > $before
            } else {
                fail "NODE change reason was not reported after changing $config"
            }

            set before [dict get [topo_stats 0] node]
            R 0 config set $config $default
            wait_for_condition 50 100 {
                [dict get [topo_stats 0] node] > $before
            } else {
                fail "NODE change reason was not reported after resetting $config"
            }
        }
    }

    test "ClusterTopologyChange does not report NODE for local cluster bus port changes" {
        set before [dict get [topo_stats 0] node]
        R 0 config set cluster-announce-bus-port 32003
        assert_equal $before [dict get [topo_stats 0] node]

        R 0 config set cluster-announce-bus-port 0
        assert_equal $before [dict get [topo_stats 0] node]
    }

    if {$::tls} {
        test "ClusterTopologyChange reports the NODE reason when tls-cluster changes the preferred port" {
            set tls_port [lindex [R 0 config get tls-port] 1]
            set tcp_port [lindex [R 0 config get port] 1]
            assert {$tls_port != $tcp_port}

            set before [dict get [topo_stats 0] node]
            R 0 config set tls-cluster no
            wait_for_condition 50 100 {
                [dict get [topo_stats 0] node] > $before
            } else {
                fail "NODE change reason was not reported when tls-cluster was disabled"
            }

            set before [dict get [topo_stats 0] node]
            R 0 config set tls-cluster yes
            wait_for_condition 50 100 {
                [dict get [topo_stats 0] node] > $before
            } else {
                fail "NODE change reason was not reported when tls-cluster was enabled"
            }
        }
    }
}
