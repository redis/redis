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

}
