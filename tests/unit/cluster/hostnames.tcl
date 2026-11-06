#
# Copyright (c) 2009-Present, Redis Ltd.
# All rights reserved.
#
# Copyright (c) 2024-present, Valkey contributors.
# All rights reserved.
#
# Licensed under your choice of (a) the Redis Source Available License 2.0
# (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
# GNU Affero General Public License v3 (AGPLv3).
#
# Portions of this file are available under BSD3 terms; see REDISCONTRIBUTIONS for more information.
#

proc get_slot_field {slot_output shard_id node_id attrib_id} {
    return [lindex [lindex [lindex $slot_output $shard_id] $node_id] $attrib_id]
}

proc build_cluster_bus_ping {sender_name sender_port sender_cport extensions_data} {
    set CLUSTERMSG_TYPE_PING 0
    set CLUSTERMSG_FLAG0_EXT_DATA 0x04
    set CLUSTER_NODE_MASTER 1

    set num_extensions [expr {[string length $extensions_data] > 0 ? 1 : 0}]
    set mflags0 [expr {$num_extensions > 0 ? $CLUSTERMSG_FLAG0_EXT_DATA : 0}]
    set totlen [expr {2256 + [string length $extensions_data]}]

    set hdr [build_cluster_bus_header $sender_name $sender_port $sender_cport \
        $CLUSTERMSG_TYPE_PING $totlen $num_extensions $CLUSTER_NODE_MASTER $mflags0]
    append hdr $extensions_data
    return $hdr
}

proc build_hostname_extension {hostname_bytes} {
    set ext_header_size 8
    set total_ext_len [expr {$ext_header_size + [string length $hostname_bytes]}]
    set padded_len [expr {(($total_ext_len + 7) / 8) * 8}]
    set padding_len [expr {$padded_len - $total_ext_len}]

    set ext ""
    append ext [binary format I $padded_len]
    append ext [binary format S 0]
    append ext [binary format S 0]
    append ext $hostname_bytes
    append ext [string repeat \x00 $padding_len]
    return $ext
}

# Start a cluster with 3 masters and 4 replicas.
# These tests rely on specific node ordering, so make sure no node fails over.
start_cluster 3 4 {tags {external:skip cluster} overrides {cluster-replica-no-failover yes}} {
test "Set cluster hostnames and verify they are propagated" {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
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
    for {set j 0} {$j < [llength $::servers]} {incr j} {
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
    for {set j 0} {$j < [llength $::servers]} {incr j} {
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

    set primaries 3
    for {set j 0} {$j < $primaries} {incr j} {
        # Set hostnames for the masters, now that the node is isolated
        R $j config set cluster-announce-hostname "shard-$j.com"
    }

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
    wait_for_condition 50 100 {
        [lindex [get_slot_field [R 6 CLUSTER SLOTS] 0 2 3] 1] eq "shard-1.com"
    } else {
        fail "hostname for shard-1 didn't reach node 6"
    }

    wait_for_condition 50 100 {
        [lindex [get_slot_field [R 6 CLUSTER SLOTS] 1 2 3] 1] eq "shard-2.com"
    } else {
        fail "hostname for shard-2 didn't reach node 6"
    }

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

    for {set j 0} {$j < $primaries} {incr j} {
        wait_for_condition 50 100 {
            [lindex [get_slot_field [R 6 CLUSTER SLOTS] $j 2 3] 1] eq "shard-$j.com"
        } else {
            fail "hostname information for shard-$j didn't reach node 6"
        }
    }
}

test "Test restart will keep hostname information" {
    # Set a new hostname, reboot and make sure it sticks
    R 0 config set cluster-announce-hostname "restart-1.com"
    
    # Store the hostname in the config
    R 0 config rewrite

    restart_server 0 true false
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

test "PING with hostname extension missing null terminator is rejected" {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        R $j config set cluster-announce-hostname ""
    }
    wait_for_condition 50 100 {
        [are_hostnames_propagated ""] eq 1
    } else {
        fail "hostnames were not cleared"
    }

    set node1_id [R 1 CLUSTER MYID]
    set target_host [srv 0 host]
    set target_bus_port [expr {[srv 0 port] + 10000}]
    set sender_port [srv -1 port]
    set sender_cport [expr {$sender_port + 10000}]

    # Freeze node 1 so its real PINGs cannot overwrite the injected hostname.
    set node1_pid [srv -1 pid]
    pause_process $node1_pid

    set payload_len 32
    set bad_hostname [string repeat A $payload_len]
    set bad_ext [build_hostname_extension $bad_hostname]
    set bad_packet [build_cluster_bus_ping $node1_id $sender_port $sender_cport $bad_ext]

    # Record the log position before injecting the bad packet.
    set loglines [count_log_lines 0]

    if {$::tls} {
        set fd [::tls::socket \
            -cafile "$::tlsdir/ca.crt" \
            -certfile "$::tlsdir/client.crt" \
            -keyfile "$::tlsdir/client.key" \
            $target_host $target_bus_port]
    } else {
        set fd [socket $target_host $target_bus_port]
    }
    fconfigure $fd -translation binary -buffering full
    puts -nonewline $fd $bad_packet
    flush $fd

    # Verify the server logged the proper rejection message.
    wait_for_log_messages 0 {"*missing null terminator*"} $loglines 50 100

    close $fd
    resume_process $node1_pid
}
}
