# Return the "ip:port@cport[,hostname]" field that server 'srv_idx' shows for
# 'node_id' in CLUSTER NODES.
proc cluster_node_address {srv_idx node_id} {
    foreach line [split [R $srv_idx CLUSTER NODES] "\n"] {
        if {[lindex [split $line " "] 0] eq $node_id} {
            return [lindex [split $line " "] 1]
        }
    }
    fail "Node $node_id not found in CLUSTER NODES of server $srv_idx"
}

start_cluster 2 0 {tags {external:skip cluster}} {

test "PUBLISH with channel_len + message_len overflow is rejected" {
    set CLUSTERMSG_TYPE_PUBLISH 4
    set CLUSTERMSG_MIN_LEN 2256

    set target_host [srv 0 host]
    set target_bus_port [expr {[srv 0 port] + 10000}]
    set node_id [R 1 CLUSTER MYID]
    set sender_port [srv -1 port]
    set sender_cport [expr {$sender_port + 10000}]

    # sizeof(clusterMsgDataPublish) - 8 = 16 - 8 = 8
    # With channel_len=0x80000000 and message_len=0x80000000, their sum
    # overflows uint32 to 0, making explen = 2256 + 8 = 2264 without the fix.
    # The fix detects the overflow before computing explen.
    set channel_len 0x80000000
    set message_len 0x80000000
    set totlen [expr {$CLUSTERMSG_MIN_LEN + 8}] ;# 2264

    set packet [build_cluster_bus_header $node_id $sender_port $sender_cport \
        $CLUSTERMSG_TYPE_PUBLISH $totlen]
    append packet [binary format I $channel_len]
    append packet [binary format I $message_len]

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
    puts -nonewline $fd $packet
    flush $fd

    # The fix rejects the packet and logs a warning about the overflow.
    wait_for_log_messages 0 {"*publish*overflow in length fields*"} 0 20 500
    close $fd

    # Verify the server is still responsive after the malformed packet.
    assert_equal [R 0 PING] {PONG}
}

test "MODULE with len overflow is rejected" {
    set CLUSTERMSG_TYPE_MODULE 9
    set CLUSTERMSG_MIN_LEN 2256

    set target_host [srv 0 host]
    set target_bus_port [expr {[srv 0 port] + 10000}]
    set node_id [R 1 CLUSTER MYID]
    set sender_port [srv -1 port]
    set sender_cport [expr {$sender_port + 10000}]

    # sizeof(clusterMsgModule) - 3 = 16 - 3 = 13
    # With module_len=0xFFFFFFFF, adding to base explen (2256+13=2269)
    # overflows uint32, wrapping to 2268 without the fix.
    # The fix detects the overflow before computing explen.
    set module_len 0xFFFFFFFF
    set totlen [expr {($CLUSTERMSG_MIN_LEN + 13 + $module_len) & 0xFFFFFFFF}] ;# 2268

    set packet [build_cluster_bus_header $node_id $sender_port $sender_cport \
        $CLUSTERMSG_TYPE_MODULE $totlen]
    append packet [binary format W 0]           ;# module_id
    append packet [binary format I $module_len] ;# len

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
    puts -nonewline $fd $packet
    flush $fd

    # The fix rejects the packet and logs a warning about the overflow.
    wait_for_log_messages 0 {"*module*overflow in length field*"} 0 20 500
    close $fd

    # Verify the server is still responsive after the malformed packet.
    assert_equal [R 0 PING] {PONG}
}

test "FORGOTTEN_NODE cannot delete the active packet sender" {
    set CLUSTERMSG_TYPE_PING 0
    set CLUSTERMSG_EXT_TYPE_FORGOTTEN_NODE 2
    set CLUSTERMSG_FLAG0_EXT_DATA 4
    set CLUSTER_NODE_MASTER 1
    set CLUSTERMSG_MIN_LEN 2256
    set FORGOTTEN_NODE_EXT_LEN 56

    set target_host [srv 0 host]
    set target_bus_port [expr {[srv 0 port] + 10000}]
    set node_id [R 1 CLUSTER MYID]
    set sender_port [srv -1 port]
    set sender_cport [expr {$sender_port + 10000}]
    set totlen [expr {$CLUSTERMSG_MIN_LEN + $FORGOTTEN_NODE_EXT_LEN}]

    # build_cluster_bus_header already encodes 'slaveof' as 40 NUL bytes
    # which is CLUSTER_NODE_MASTER so the sender is treated like master
    # and packet doesnt alter its slot ownership
    set packet [build_cluster_bus_header $node_id $sender_port $sender_cport \
        $CLUSTERMSG_TYPE_PING $totlen 1 $CLUSTER_NODE_MASTER $CLUSTERMSG_FLAG0_EXT_DATA]
    append packet [binary format I $FORGOTTEN_NODE_EXT_LEN]
    append packet [binary format S $CLUSTERMSG_EXT_TYPE_FORGOTTEN_NODE]
    append packet [binary format S 0]               ;# unused
    append packet [binary format a40 $node_id]
    append packet [binary format W 60]              ;# blacklist TTL

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
    puts -nonewline $fd $packet
    flush $fd

    wait_for_log_messages 0 {"*Ignoring FORGOTTEN_NODE for active sender*"} 0 20 500
    close $fd

    # The sender must still be part of the cluster, with its slots intact.
    assert_equal [R 0 PING] {PONG}
    assert_match "*$node_id*" [R 0 CLUSTER NODES]
    wait_for_cluster_state "ok"
}


test "PING with an invalid sender address is rejected" {
    set CLUSTERMSG_TYPE_PING 0
    set CLUSTER_NODE_MASTER 1
    set CLUSTERMSG_MIN_LEN 2256

    set node_id [R 1 CLUSTER MYID]
    set sender_port [srv -1 port]
    set sender_cport [expr {$sender_port + 10000}]
    set addr_before [cluster_node_address 0 $node_id]

    # Freeze node 1 so that its own PINGs cannot restore its address, which
    # would hide an accepted packet.
    set node1_pid [srv -1 pid]
    pause_process $node1_pid

    # A CRLF that would end up in -MOVED replies and split the nodes.conf
    # line, characters valid in neither an IP nor a hostname, and a field
    # without a NUL terminator in its 46 bytes.
    set bad_ips [list "127.0.0.1\r\n+PWN" "127.0.0.1 x" "host:1" \
                      [string repeat 1 46]]
    foreach bad_ip $bad_ips {
        set loglines [count_log_lines 0]
        set packet [build_cluster_bus_header $node_id $sender_port $sender_cport \
            $CLUSTERMSG_TYPE_PING $CLUSTERMSG_MIN_LEN 0 $CLUSTER_NODE_MASTER 0 $bad_ip]
        set fd [send_cluster_bus_packet 0 $packet]
        wait_for_log_messages 0 {"*Received ping packet with invalid sender address*"} $loglines 50 100
        close $fd
        assert_equal $addr_before [cluster_node_address 0 $node_id]
    }

    resume_process $node1_pid
    wait_for_cluster_state ok
}

test "PING announcing a hostname as sender address is accepted" {
    set CLUSTERMSG_TYPE_PING 0
    set CLUSTER_NODE_MASTER 1
    set CLUSTERMSG_MIN_LEN 2256

    set node_id [R 1 CLUSTER MYID]
    set sender_port [srv -1 port]
    set sender_cport [expr {$sender_port + 10000}]
    set addr_before [cluster_node_address 0 $node_id]

    # cluster-announce-ip accepts a hostname, so the bus must keep accepting
    # one. Freeze node 1 so that the announced address stays observable.
    set node1_pid [srv -1 pid]
    pause_process $node1_pid

    set packet [build_cluster_bus_header $node_id $sender_port $sender_cport \
        $CLUSTERMSG_TYPE_PING $CLUSTERMSG_MIN_LEN 0 $CLUSTER_NODE_MASTER 0 "localhost"]
    set fd [send_cluster_bus_packet 0 $packet]
    wait_for_condition 50 100 {
        [string match "localhost:*" [cluster_node_address 0 $node_id]]
    } else {
        fail "The announced hostname was not accepted: [cluster_node_address 0 $node_id]"
    }
    close $fd

    # Node 1's own PINGs announce no address, so node 0 goes back to the
    # socket address.
    resume_process $node1_pid
    wait_for_condition 50 100 {
        [cluster_node_address 0 $node_id] eq $addr_before
    } else {
        fail "Node 1 address was not restored: [cluster_node_address 0 $node_id]"
    }
    wait_for_cluster_state ok
}

test "PING with an invalid address in the gossip section is rejected" {
    set CLUSTERMSG_TYPE_PING 0
    set CLUSTER_NODE_MASTER 1
    set CLUSTERMSG_MIN_LEN 2256
    set GOSSIP_ENTRY_LEN 104

    set node_id [R 1 CLUSTER MYID]
    set sender_port [srv -1 port]
    set sender_cport [expr {$sender_port + 10000}]

    # Gossip about a node unknown to node 0, which node 0 would otherwise add
    # with the gossiped address.
    set gossiped_id [string repeat abcd1234 5]
    set totlen [expr {$CLUSTERMSG_MIN_LEN + $GOSSIP_ENTRY_LEN}]

    set loglines [count_log_lines 0]
    set packet [build_cluster_bus_header $node_id $sender_port $sender_cport \
        $CLUSTERMSG_TYPE_PING $totlen 0 $CLUSTER_NODE_MASTER 0 "" 1]
    append packet [build_cluster_bus_gossip_entry $gossiped_id \
        "127.0.0.1\r\n+PWN" 30000 40000 $CLUSTER_NODE_MASTER]
    set fd [send_cluster_bus_packet 0 $packet]
    wait_for_log_messages 0 {"*Received ping packet with invalid address in gossip section*"} $loglines 50 100
    close $fd

    assert_no_match "*$gossiped_id*" [R 0 CLUSTER NODES]
    wait_for_cluster_state ok
}

}
