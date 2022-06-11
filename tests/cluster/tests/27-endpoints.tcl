source "../tests/includes/init-tests.tcl"

# Isolate a node from the cluster and give it a new nodeid
proc isolate_node {id} {
    set node_id [R $id CLUSTER MYID]
    R 6 CLUSTER RESET HARD
    for {set j 0} {$j < 20} {incr j} {
        if { $j eq $id } {
            continue
        }
        R $j CLUSTER FORGET $node_id
    }
}

proc get_slot_field {slot_output shard_id node_id attrib_id} {
    return [lindex [lindex [lindex $slot_output $shard_id] $node_id] $attrib_id]
}

test "Create a 6 nodes cluster" {
    cluster_create_with_continuous_slots 3 3
}

test "Cluster should start ok" {
    assert_cluster_state ok
    wait_for_cluster_propagation
}

test "Set cluster hostnames and verify they are propagated" {
    for {set j 0} {$j < $::cluster_master_nodes + $::cluster_replica_nodes} {incr j} {
        R $j config set cluster-announce-hostname "host-$j.com"
    }
    
    wait_for_condition 50 100 {
        [are_hostnames_propagated "host-*.com"] eq 1
    } else {
        fail "cluster hostnames were not propagated"
    }

    # Now that everything is propagated, assert everyone agrees
    wait_for_cluster_propagation
}

test "Update hostnames and make sure they are all eventually propagated" {
    for {set j 0} {$j < $::cluster_master_nodes + $::cluster_replica_nodes} {incr j} {
        R $j config set cluster-announce-hostname "host-updated-$j.com"
    }
    
    wait_for_condition 50 100 {
        [are_hostnames_propagated "host-updated-*.com"] eq 1
    } else {
        fail "cluster hostnames were not propagated"
    }

    # Now that everything is propagated, assert everyone agrees
    wait_for_cluster_propagation
}

test "Remove hostnames and make sure they are all eventually propagated" {
    for {set j 0} {$j < $::cluster_master_nodes + $::cluster_replica_nodes} {incr j} {
        R $j config set cluster-announce-hostname ""
    }
    
    wait_for_condition 50 100 {
        [are_hostnames_propagated ""] eq 1
    } else {
        fail "cluster hostnames were not propagated"
    }

    # Now that everything is propagated, assert everyone agrees
    wait_for_cluster_propagation
}

test "Verify cluster-preferred-endpoint-type behavior for redirects and info" {
    R 0 config set cluster-announce-hostname "me.com"
    R 1 config set cluster-announce-hostname ""
    R 2 config set cluster-announce-hostname "them.com"

    wait_for_cluster_propagation

    # Verify default behavior
    set slot_result [R 0 cluster slots]
    assert_equal "" [lindex [get_slot_field $slot_result 0 2 0] 1]
    assert_equal "" [lindex [get_slot_field $slot_result 2 2 0] 1]
    assert_equal "hostname" [lindex [get_slot_field $slot_result 0 2 3] 0]
    assert_equal "me.com" [lindex [get_slot_field $slot_result 0 2 3] 1]
    assert_equal "hostname" [lindex [get_slot_field $slot_result 2 2 3] 0]
    assert_equal "them.com" [lindex [get_slot_field $slot_result 2 2 3] 1]

    # Redirect will use the IP address
    catch {R 0 set foo foo} redir_err
    assert_match "MOVED * 127.0.0.1:*" $redir_err

    # Verify prefer hostname behavior
    R 0 config set cluster-preferred-endpoint-type hostname

    set slot_result [R 0 cluster slots]
    assert_equal "me.com" [get_slot_field $slot_result 0 2 0]
    assert_equal "them.com" [get_slot_field $slot_result 2 2 0]

    # Redirect should use hostname
    catch {R 0 set foo foo} redir_err
    assert_match "MOVED * them.com:*" $redir_err

    # Redirect to an unknown hostname returns ?
    catch {R 0 set barfoo bar} redir_err
    assert_match "MOVED * ?:*" $redir_err

    # Verify unknown hostname behavior
    R 0 config set cluster-preferred-endpoint-type unknown-endpoint

    # Verify default behavior
    set slot_result [R 0 cluster slots]
    assert_equal "ip" [lindex [get_slot_field $slot_result 0 2 3] 0]
    assert_equal "127.0.0.1" [lindex [get_slot_field $slot_result 0 2 3] 1]
    assert_equal "ip" [lindex [get_slot_field $slot_result 2 2 3] 0]
    assert_equal "127.0.0.1" [lindex [get_slot_field $slot_result 2 2 3] 1]
    assert_equal "ip" [lindex [get_slot_field $slot_result 1 2 3] 0]
    assert_equal "127.0.0.1" [lindex [get_slot_field $slot_result 1 2 3] 1]
    # Not required by the protocol, but IP comes before hostname
    assert_equal "hostname" [lindex [get_slot_field $slot_result 0 2 3] 2]
    assert_equal "me.com" [lindex [get_slot_field $slot_result 0 2 3] 3]
    assert_equal "hostname" [lindex [get_slot_field $slot_result 2 2 3] 2]
    assert_equal "them.com" [lindex [get_slot_field $slot_result 2 2 3] 3]

    # This node doesn't have a hostname
    assert_equal 2 [llength [get_slot_field $slot_result 1 2 3]]

    # Redirect should use empty string
    catch {R 0 set foo foo} redir_err
    assert_match "MOVED * :*" $redir_err

    R 0 config set cluster-preferred-endpoint-type ip
}

test "Verify the nodes configured with prefer hostname only show hostname for new nodes" {
    # Have everyone forget node 6 and isolate it from the cluster.
    isolate_node 6

    # Set hostnames for the primaries, now that the node is isolated
    R 0 config set cluster-announce-hostname "shard-1.com"
    R 1 config set cluster-announce-hostname "shard-2.com"
    R 2 config set cluster-announce-hostname "shard-3.com"

    # Prevent Node 0 and Node 6 from properly meeting,
    # they'll hang in the handshake phase. This allows us to 
    # test the case where we "know" about it but haven't
    # successfully retrieved information about it yet.
    R 0 DEBUG DROP-CLUSTER-PACKET-FILTER 0
    R 6 DEBUG DROP-CLUSTER-PACKET-FILTER 0

    # Have a replica meet the isolated node
    R 3 cluster meet 127.0.0.1 [get_instance_attrib redis 6 port]

    # Wait for the isolated node to learn about the rest of the cluster,
    # which correspond to a single entry in cluster nodes. Note this
    # doesn't mean the isolated node has successfully contacted each
    # node.
    wait_for_condition 50 100 {
        [llength [split [R 6 CLUSTER NODES] "\n"]] eq 21 
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
    assert_equal [lindex [get_slot_field $slot_result 0 2 3] 1] "shard-2.com"
    assert_equal [lindex [get_slot_field $slot_result 1 2 3] 1] "shard-3.com"

    # Also make sure we know about the isolated primary, we 
    # just can't reach it.
    set primary_id [R 0 CLUSTER MYID]
    assert_match "*$primary_id*" [R 6 CLUSTER NODES]

    # Stop dropping cluster packets, and make sure everything
    # stabilizes
    R 0 DEBUG DROP-CLUSTER-PACKET-FILTER -1
    R 6 DEBUG DROP-CLUSTER-PACKET-FILTER -1

    wait_for_condition 50 100 {
        [llength [R 6 CLUSTER SLOTS]] eq 3
    } else {
        fail "Node did not learn about the 2 shards it can talk to"
    }
    set slot_result [R 6 CLUSTER SLOTS]
    assert_equal [lindex [get_slot_field $slot_result 0 2 3] 1] "shard-1.com"
    assert_equal [lindex [get_slot_field $slot_result 1 2 3] 1] "shard-2.com"
    assert_equal [lindex [get_slot_field $slot_result 2 2 3] 1] "shard-3.com"
}

test "Test restart will keep hostname information" {
    # Set a new hostname, reboot and make sure it sticks
    R 0 config set cluster-announce-hostname "restart-1.com"
    # Store the hostname in the config
    R 0 config rewrite
    kill_instance redis 0
    restart_instance redis 0
    set slot_result [R 0 CLUSTER SLOTS]
    assert_equal [lindex [get_slot_field $slot_result 0 2 3] 1] "restart-1.com"

    # As a sanity check, make sure everyone eventually agrees
    wait_for_cluster_propagation
}

test "Test hostname validation" {
    catch {R 0 config set cluster-announce-hostname [string repeat x 256]} err
    assert_match "*Hostnames must be less than 256 characters*" $err
    catch {R 0 config set cluster-announce-hostname "?.com"} err
    assert_match "*Hostnames may only contain alphanumeric characters, hyphens or dots*" $err

    # Note this isn't a valid hostname, but it passes our internal validation
    R 0 config set cluster-announce-hostname "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-."
}