# Isolate a node from the cluster and give it a new nodeid
proc isolate_node {id} {
    set node_id [r $id CLUSTER MYID]
    r $id CLUSTER rESET HARD
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        if { $j eq $id } {
            continue
        }
        r $j CLUSTER FORGET $node_id
    }
}

# Check if cluster's view of hostnames is consistent
proc are_hostnames_propagated {match_string} {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        set cfg [r $j cluster slots]
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
start_cluster 3 4 {tags {external:skip cluster}} {
test "Set cluster hostnames and verify they are propagated" {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        r $j config set cluster-announce-hostname "host-$j.com"
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
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        r $j config set cluster-announce-hostname "host-updated-$j.com"
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
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        r $j config set cluster-announce-hostname ""
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
    r 0 config set cluster-announce-hostname "me.com"
    r 1 config set cluster-announce-hostname ""
    r 2 config set cluster-announce-hostname "them.com"

    wait_for_cluster_propagation

    # Verify default behavior
    set slot_result [r 0 cluster slots]
    assert_equal "" [lindex [get_slot_field $slot_result 0 2 0] 1]
    assert_equal "" [lindex [get_slot_field $slot_result 2 2 0] 1]
    assert_equal "hostname" [lindex [get_slot_field $slot_result 0 2 3] 0]
    assert_equal "me.com" [lindex [get_slot_field $slot_result 0 2 3] 1]
    assert_equal "hostname" [lindex [get_slot_field $slot_result 2 2 3] 0]
    assert_equal "them.com" [lindex [get_slot_field $slot_result 2 2 3] 1]

    # redirect will use the IP address
    catch {r 0 set foo foo} redir_err
    assert_match "MOVED * 127.0.0.1:*" $redir_err

    # Verify prefer hostname behavior
    r 0 config set cluster-preferred-endpoint-type hostname

    set slot_result [r 0 cluster slots]
    assert_equal "me.com" [get_slot_field $slot_result 0 2 0]
    assert_equal "them.com" [get_slot_field $slot_result 2 2 0]

    # redirect should use hostname
    catch {r 0 set foo foo} redir_err
    assert_match "MOVED * them.com:*" $redir_err

    # redirect to an unknown hostname returns ?
    catch {r 0 set barfoo bar} redir_err
    assert_match "MOVED * ?:*" $redir_err

    # Verify unknown hostname behavior
    r 0 config set cluster-preferred-endpoint-type unknown-endpoint

    # Verify default behavior
    set slot_result [r 0 cluster slots]
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

    # redirect should use empty string
    catch {r 0 set foo foo} redir_err
    assert_match "MOVED * :*" $redir_err

    r 0 config set cluster-preferred-endpoint-type ip
}

test "Verify the nodes configured with prefer hostname only show hostname for new nodes" {
    # Have everyone forget node 6 and isolate it from the cluster.
    isolate_node 6

    # Set hostnames for the masters, now that the node is isolated
    r 0 config set cluster-announce-hostname "shard-1.com"
    r 1 config set cluster-announce-hostname "shard-2.com"
    r 2 config set cluster-announce-hostname "shard-3.com"

    # Prevent Node 0 and Node 6 from properly meeting,
    # they'll hang in the handshake phase. This allows us to 
    # test the case where we "know" about it but haven't
    # successfully retrieved information about it yet.
    r 0 DEBUG DROP-CLUSTER-PACKET-FILTER 0
    r 6 DEBUG DROP-CLUSTER-PACKET-FILTER 0

    # Have a replica meet the isolated node
    r 3 cluster meet 127.0.0.1 [srv -6 port]

    # Wait for the isolated node to learn about the rest of the cluster,
    # which correspond to a single entry in cluster nodes. Note this
    # doesn't mean the isolated node has successfully contacted each
    # node.
    wait_for_condition 50 100 {
        [llength [split [r 6 CLUSTER NODES] "\n"]] eq [expr [llength $::servers] + 1]
    } else {
        fail "Isolated node didn't learn about the rest of the cluster *"
    }

    # Now, we wait until the two nodes that aren't filtering packets
    # to accept our isolated nodes connections. At this point they will
    # start showing up in cluster slots. 
    wait_for_condition 50 100 {
        [llength [r 6 CLUSTER SLOTS]] eq 2
    } else {
        fail "Node did not learn about the 2 shards it can talk to"
    }
    set slot_result [r 6 CLUSTER SLOTS]
    assert_equal [lindex [get_slot_field $slot_result 0 2 3] 1] "shard-2.com"
    assert_equal [lindex [get_slot_field $slot_result 1 2 3] 1] "shard-3.com"

    # Also make sure we know about the isolated master, we 
    # just can't reach it.
    set master_id [r 0 CLUSTER MYID]
    assert_match "*$master_id*" [r 6 CLUSTER NODES]

    # Stop dropping cluster packets, and make sure everything
    # stabilizes
    r 0 DEBUG DROP-CLUSTER-PACKET-FILTER -1
    r 6 DEBUG DROP-CLUSTER-PACKET-FILTER -1

    wait_for_condition 50 100 {
        [llength [r 6 CLUSTER SLOTS]] eq 3
    } else {
        fail "Node did not learn about the 2 shards it can talk to"
    }
    set slot_result [r 6 CLUSTER SLOTS]
    assert_equal [lindex [get_slot_field $slot_result 0 2 3] 1] "shard-1.com"
    assert_equal [lindex [get_slot_field $slot_result 1 2 3] 1] "shard-2.com"
    assert_equal [lindex [get_slot_field $slot_result 2 2 3] 1] "shard-3.com"
}

test "Test restart will keep hostname information" {
    # Set a new hostname, reboot and make sure it sticks
    r 0 config set cluster-announce-hostname "restart-1.com"
    # Store the hostname in the config
    r 0 config rewrite
    restart_server 0 true false
    set slot_result [r 0 CLUSTER SLOTS]
    assert_equal [lindex [get_slot_field $slot_result 0 2 3] 1] "restart-1.com"

    # As a sanity check, make sure everyone eventually agrees
    wait_for_cluster_propagation
}

test "Test hostname validation" {
    catch {r 0 config set cluster-announce-hostname [string repeat x 256]} err
    assert_match "*Hostnames must be less than 256 characters*" $err
    catch {r 0 config set cluster-announce-hostname "?.com"} err
    assert_match "*Hostnames may only contain alphanumeric characters, hyphens or dots*" $err

    # Note this isn't a valid hostname, but it passes our internal validation
    r 0 config set cluster-announce-hostname "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-."
}
}
