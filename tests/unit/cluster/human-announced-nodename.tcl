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
        set cfg [R $j cluster slots]
        foreach node $cfg {
            for {set i 2} {$i < [llength $node]} {incr i} {
                if {! [string match $match_string [lindex [lindex [lindex $node $i] 3] 1]] } {
                    return 0
                }
            }
        }
    }
    return 1
}

proc get_slot_field {slot_output shard_id node_id attrib_id} {
    return [lindex [lindex [lindex $slot_output $shard_id] $node_id] $attrib_id]
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

test "Verify cluster-preferred-endpoint-type behavior for redirects and info" {
    R 0 config set cluster-announce-human-nodename "nodenameone.com"
    R 1 config set cluster-announce-human-nodename ""
    R 2 config set cluster-announce-human-nodename "nodenametwo.com"

    wait_for_cluster_propagation

    # Verify default behavior
    set slot_result [R 0 cluster slots]
    assert_equal "" [lindex [get_slot_field $slot_result 0 2 0] 1]
    assert_equal "" [lindex [get_slot_field $slot_result 2 2 0] 1]
    assert_equal "nodename" [lindex [get_slot_field $slot_result 0 2 3] 0]
    assert_equal "nodenameone.com" [lindex [get_slot_field $slot_result 0 2 3] 1]
    assert_equal "nodename" [lindex [get_slot_field $slot_result 2 2 3] 0]
    assert_equal "nodenametwo.com" [lindex [get_slot_field $slot_result 2 2 3] 1]

    # Redirect will use the IP address
    catch {R 0 set foo foo} redir_err
    assert_match "MOVED * 127.0.0.1:*" $redir_err

    # Verify prefer nodename behavior
    R 0 config set cluster-preferred-endpoint-type nodename

    set slot_result [R 0 cluster slots]
    assert_equal "nodenameone.com" [get_slot_field $slot_result 0 2 0]
    assert_equal "nodenametwo.com" [get_slot_field $slot_result 2 2 0]

    # Redirect should use nodename
    catch {R 0 set foo foo} redir_err
    assert_match "MOVED * nodenametwo.com:*" $redir_err

    # Redirect to an unknown nodename returns ?
    catch {R 0 set barfoo bar} redir_err
    assert_match "MOVED * ?:*" $redir_err

    # Verify unknown nodename behavior
    R 0 config set cluster-preferred-endpoint-type unknown-endpoint

    # Verify default behavior
    set slot_result [R 0 cluster slots]
    assert_equal "ip" [lindex [get_slot_field $slot_result 0 2 3] 0]
    assert_equal "127.0.0.1" [lindex [get_slot_field $slot_result 0 2 3] 1]
    assert_equal "ip" [lindex [get_slot_field $slot_result 2 2 3] 0]
    assert_equal "127.0.0.1" [lindex [get_slot_field $slot_result 2 2 3] 1]
    assert_equal "ip" [lindex [get_slot_field $slot_result 1 2 3] 0]
    assert_equal "127.0.0.1" [lindex [get_slot_field $slot_result 1 2 3] 1]
    # Not required by the protocol, but IP comes before nodename
    assert_equal "nodename" [lindex [get_slot_field $slot_result 0 2 3] 2]
    assert_equal "nodenameone.com" [lindex [get_slot_field $slot_result 0 2 3] 3]
    assert_equal "nodename" [lindex [get_slot_field $slot_result 2 2 3] 2]
    assert_equal "nodenametwo.com" [lindex [get_slot_field $slot_result 2 2 3] 3]

    # This node doesn't have a nodename
    assert_equal 2 [llength [get_slot_field $slot_result 1 2 3]]

    # Redirect should use empty string
    catch {R 0 set foo foo} redir_err
    assert_match "MOVED * :*" $redir_err

    R 0 config set cluster-preferred-endpoint-type ip
}

test "Verify the nodes configured with prefer nodename only show nodename for new nodes" {
    # Have everyone forget node 6 and isolate it from the cluster.
    isolate_node 6

    # Set nodenames for the masters, now that the node is isolated
    R 0 config set cluster-announce-human-nodename "nodename-1.com"
    R 1 config set cluster-announce-human-nodename "nodename-2.com"
    R 2 config set cluster-announce-human-nodename "nodename-3.com"

    # Prevent Node 0 and Node 6 from properly meeting,
    # they'll hang in the handshake phase. This allows us to 
    # test the case where we "know" about it but haven't
    # successfully retrieved information about it yet.
    R 0 DEBUG DROP-CLUSTER-PACKET-FILTER 0
    R 6 DEBUG DROP-CLUSTER-PACKET-FILTER 0

    # Have a replica meet the isolated node
    R 3 cluster meet 127.0.0.1 [srv -6 port]

    # Wait for the isolated node to learn about the rest of the cluster,
    # which correspond to a single entry in cluster nodes. Note this
    # doesn't mean the isolated node has successfully contacted each
    # node.
    wait_for_condition 50 100 {
        [llength [split [R 6 CLUSTER NODES] "\n"]] eq [expr [llength $::servers] + 1]
    } else {
        fail "Isolated node didn't learn about the rest of the cluster *"
    }

    # Now, we wait until the two nodes that aren't filtering packets
    # to accept our isolated nodes connections. At this point they will
    # start showing up in cluster slots. 
    wait_for_condition 50 100 {
        [llength [R 6 CLUSTER SLOTS]] eq 2
    } else {
        fail "Node did not learn about the 2 shards it can talk to"
    }
    set slot_result [R 6 CLUSTER SLOTS]
    assert_equal [lindex [get_slot_field $slot_result 0 2 3] 1] "nodename-2.com"
    assert_equal [lindex [get_slot_field $slot_result 1 2 3] 1] "nodename-3.com"

    # Also make sure we know about the isolated master, we 
    # just can't reach it.
    set master_id [R 0 CLUSTER MYID]
    assert_match "*$master_id*" [R 6 CLUSTER NODES]

    # Stop dropping cluster packets, and make sure everything
    # stabilizes
    R 0 DEBUG DROP-CLUSTER-PACKET-FILTER -1
    R 6 DEBUG DROP-CLUSTER-PACKET-FILTER -1

    # This operation sometimes spikes to around 5 seconds to resolve the state,
    # so it has a higher timeout. 
    wait_for_condition 50 500 {
        [llength [R 6 CLUSTER SLOTS]] eq 3
    } else {
        fail "Node did not learn about the 2 shards it can talk to"
    }
    set slot_result [R 6 CLUSTER SLOTS]
    assert_equal [lindex [get_slot_field $slot_result 0 2 3] 1] "nodename-1.com"
    assert_equal [lindex [get_slot_field $slot_result 1 2 3] 1] "nodename-2.com"
    assert_equal [lindex [get_slot_field $slot_result 2 2 3] 1] "nodename-3.com"
}

test "Test restart will keep nodename information" {
    # Set a new nodename, reboot and make sure it sticks
    R 0 config set cluster-announce-human-nodename "restart-1.com"

    # Store the nodename in the config
    R 0 config rewrite

    restart_server 0 true false
    set slot_result [R 0 CLUSTER SLOTS]
    assert_equal [lindex [get_slot_field $slot_result 0 2 3] 1] "restart-1.com"

    # As a sanity check, make sure everyone eventually agrees
    wait_for_cluster_propagation
}

test "Test human announced nodename validation" {
    catch {R 0 config set cluster-announce-human-nodename [string repeat x 256]} err
    assert_match "*nodename must be less than 256 characters*" $err
    catch {R 0 config set cluster-announce-human-nodename "?.com"} err
    assert_match "*nodename may only contain alphanumeric characters, hyphens, dots or underscore*" $err

    # Note this isn't a valid nodename, but it passes our internal validation
    R 0 config set cluster-announce-human-nodename "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-."
}
}
