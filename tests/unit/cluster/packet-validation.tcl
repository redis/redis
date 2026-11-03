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

    # Control characters: a CRLF that would end up in -MOVED replies and split
    # the nodes.conf line, a tab and DEL. Field delimiters in nodes.conf and
    # CLUSTER NODES: space and comma. Both quote characters, which nodes.conf
    # parsing treats specially. Each square bracket on its own, since brackets
    # have no use in an address and confuse redis-cli's parsing of CLUSTER
    # NODES. And 46 bytes without a NUL terminator.
    set bad_ips [list "127.0.0.1\r\n+PWN" "host\t1" "host[format %c 127]1" \
                      "127.0.0.1 x" "host,1" "host\"1" "host'1" \
                      "\[host" "host\]" "\[::1\]:1" [string repeat 1 46]]
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

# Addresses announced by the forged packets below. Node 0 stores what a peer
# announces and resolves it, synchronously, every time it tries to connect to
# the peer, so a name that needs a DNS lookup must not be used: in CI a lookup
# of an unknown name has been seen to take 5 to 10 seconds to time out, which
# stalls node 0 for that long on every attempt.
#
# The alias stands for a Docker-style name with an underscore, which the
# cluster-announce-ip validation does not accept but older nodes can announce.
# Its leading dot gives it an empty label, which both glibc and musl refuse to
# look up without sending a query, so the connection attempt fails at once.
set alias_name ".redis_node_1"
# The longest address that fits the field: 45 bytes plus the NUL.
set longest_ipv6 "2001:0db8:0000:0000:0000:ffff:192.168.100.228"

test "PING announcing a hostname or alias as sender address is accepted" {
    set CLUSTERMSG_TYPE_PING 0
    set CLUSTER_NODE_MASTER 1
    set CLUSTERMSG_MIN_LEN 2256

    set node_id [R 1 CLUSTER MYID]
    set sender_port [srv -1 port]
    set sender_cport [expr {$sender_port + 10000}]
    set addr_before [cluster_node_address 0 $node_id]

    # cluster-announce-ip accepts a hostname, so the bus must keep accepting
    # one. Nodes that predate the cluster-announce-ip validation may announce
    # names outside its character set, such as a Docker alias with an
    # underscore, and those must keep working across a rolling upgrade. The
    # longest possible field is accepted too.
    foreach name [list localhost $alias_name $longest_ipv6] {
        # Freeze node 1 so that the announced address stays observable.
        set node1_pid [srv -1 pid]
        pause_process $node1_pid

        set packet [build_cluster_bus_header $node_id $sender_port $sender_cport \
            $CLUSTERMSG_TYPE_PING $CLUSTERMSG_MIN_LEN 0 $CLUSTER_NODE_MASTER 0 $name]
        set fd [send_cluster_bus_packet 0 $packet]
        wait_for_condition 50 100 {
            [string match "$name:*" [cluster_node_address 0 $node_id]]
        } else {
            fail "The announced address $name was not accepted: [cluster_node_address 0 $node_id]"
        }
        close $fd

        # Node 1's own PINGs announce no address, so node 0 goes back to the
        # socket address.
        resume_process $node1_pid
        wait_for_condition 100 100 {
            [cluster_node_address 0 $node_id] eq $addr_before
        } else {
            fail "Node 1 address was not restored: [cluster_node_address 0 $node_id]"
        }
    }
    wait_for_cluster_state ok
}

test "PING with an invalid address in a gossip entry skips only that entry" {
    set CLUSTERMSG_TYPE_PING 0
    set CLUSTERMSG_TYPE_PONG 1
    set CLUSTER_NODE_MASTER 1
    set CLUSTERMSG_MIN_LEN 2256
    set GOSSIP_ENTRY_LEN 104
    # Offset of the 16-bit message type in the header, after the signature,
    # the total length, the version and the port.
    set CLUSTERMSG_TYPE_OFFSET 12
    set CLUSTERMSG_TYPE_END [expr {$CLUSTERMSG_TYPE_OFFSET + 2}]

    set node_id [R 1 CLUSTER MYID]
    set sender_port [srv -1 port]
    set sender_cport [expr {$sender_port + 10000}]

    # Gossip about two nodes unknown to node 0, one with an address that would
    # corrupt nodes.conf and one with a valid address. A node that holds such
    # an address relays it in every PING, so only the bad entry is ignored:
    # the good one is added and the sender still gets its PONG.
    set bad_id [string repeat abcd1234 5]
    set good_id [string repeat 5678efab 5]
    set totlen [expr {$CLUSTERMSG_MIN_LEN + 2 * $GOSSIP_ENTRY_LEN}]

    set loglines [count_log_lines 0]
    set packet [build_cluster_bus_header $node_id $sender_port $sender_cport \
        $CLUSTERMSG_TYPE_PING $totlen 0 $CLUSTER_NODE_MASTER 0 "" 2]
    append packet [build_cluster_bus_gossip_entry $bad_id \
        "127.0.0.1\r\n+PWN" 30000 40000 $CLUSTER_NODE_MASTER]
    append packet [build_cluster_bus_gossip_entry $good_id \
        "127.0.0.1" 30001 40001 $CLUSTER_NODE_MASTER]

    # Freeze node 1 so that it cannot reconnect to node 0 and replace the
    # forged connection before the PONG is read from it.
    set node1_pid [srv -1 pid]
    pause_process $node1_pid
    set failed [catch {
        set fd [send_cluster_bus_packet 0 $packet]
        fconfigure $fd -blocking 0 -translation binary
        # Collect the reply until the header is in up to the message type.
        set reply ""
        wait_for_condition 50 100 {
            [string length [append reply [read $fd]]] >= $CLUSTERMSG_TYPE_END
        } else {
            fail "Node 0 did not answer the PING"
        }
        close $fd
        # Unpack the header: the first 4 bytes are the signature, then skip
        # to the message type and read it as a big-endian 16-bit integer.
        binary scan $reply a4@${CLUSTERMSG_TYPE_OFFSET}S sig type
        # Only a cluster bus message starts with "RCmb", and the answer to a
        # PING is a PONG.
        assert_equal "RCmb" $sig
        assert_equal $CLUSTERMSG_TYPE_PONG $type
    } msg opts]
    # Resume node 1 whatever happened above, then re-raise any failure.
    resume_process $node1_pid
    if {$failed} { return -options $opts $msg }

    wait_for_log_messages 0 [list "*Ignoring gossip about node $bad_id with an invalid address*"] $loglines 50 100
    wait_for_condition 50 100 {
        [string match "*$good_id 127.0.0.1:30001@40001*" [R 0 CLUSTER NODES]]
    } else {
        fail "The valid gossip entry was not processed: [R 0 CLUSTER NODES]"
    }
    assert_no_match "*$bad_id*" [R 0 CLUSTER NODES]

    # Node 0 cannot reach the gossiped node, so it never relays it to node 1.
    # Forget it so that later tests see the original two-node cluster.
    R 0 CLUSTER FORGET $good_id
    wait_for_condition 50 100 {
        ![string match "*$good_id*" [R 0 CLUSTER NODES]]
    } else {
        fail "The gossiped node was not forgotten"
    }
    wait_for_cluster_state ok
}

test "PING gossiping an alias address is accepted, relayed and persisted" {
    set CLUSTERMSG_TYPE_PING 0
    set CLUSTER_NODE_MASTER 1
    set CLUSTERMSG_MIN_LEN 2256
    set GOSSIP_ENTRY_LEN 104

    set node_id [R 1 CLUSTER MYID]
    set sender_port [srv -1 port]
    set sender_cport [expr {$sender_port + 10000}]

    # Gossip about a node unknown to node 0, announced at the alias that the
    # cluster-announce-ip validation would not accept. Older nodes can
    # announce such names, and a rolling upgrade also depends on them being
    # accepted when relayed by other nodes.
    set alias_node_id [string repeat 1234abcd 5]
    set alias_addr "$alias_name:30000@40000"
    set totlen [expr {$CLUSTERMSG_MIN_LEN + $GOSSIP_ENTRY_LEN}]
    set packet [build_cluster_bus_header $node_id $sender_port $sender_cport \
        $CLUSTERMSG_TYPE_PING $totlen 0 $CLUSTER_NODE_MASTER 0 "" 1]
    append packet [build_cluster_bus_gossip_entry $alias_node_id \
        $alias_name 30000 40000 $CLUSTER_NODE_MASTER]
    set fd [send_cluster_bus_packet 0 $packet]
    wait_for_condition 50 100 {
        [string match "*$alias_node_id $alias_addr*" [R 0 CLUSTER NODES]]
    } else {
        fail "The gossiped alias address was not accepted: [R 0 CLUSTER NODES]"
    }
    close $fd

    # Node 0 relays the new node to node 1 in its own gossip.
    wait_for_condition 100 100 {
        [string match "*$alias_node_id $alias_addr*" [R 1 CLUSTER NODES]]
    } else {
        fail "The alias address was not relayed to node 1: [R 1 CLUSTER NODES]"
    }

    # Keeping nodes.conf loadable is the point of validating these fields:
    # the alias must be written and read back as is. Freeze node 1 across the
    # restart so that node 0 can only get the alias back from its own
    # nodes.conf, not from node 1's gossip.
    set node1_pid [srv -1 pid]
    pause_process $node1_pid
    set failed [catch {
        R 0 CLUSTER SAVECONFIG
        restart_server 0 true false
        assert_match "*$alias_node_id $alias_addr*" [R 0 CLUSTER NODES]
    } msg opts]
    # Resume node 1 whatever happened above, then re-raise any failure.
    resume_process $node1_pid
    if {$failed} { return -options $opts $msg }

    # Forget the gossiped node everywhere so that later tests see the
    # original two-node cluster.
    foreach idx {0 1} {
        catch {R $idx CLUSTER FORGET $alias_node_id}
    }
    wait_for_condition 100 100 {
        ![string match "*$alias_node_id*" [R 0 CLUSTER NODES]] &&
        ![string match "*$alias_node_id*" [R 1 CLUSTER NODES]]
    } else {
        fail "The gossiped node was not forgotten"
    }
    wait_for_cluster_state ok
}
}
