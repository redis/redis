# Validate compact cluster gossip packets and exercise discovery and failure
# propagation after peers have negotiated the compact format.

proc compact_gossip_test_extension {payload {nonzero_padding 0}} {
    set padding_length [expr {(8 - ([string length $payload] % 8)) % 8}]
    set padding [string repeat \x00 $padding_length]
    if {$nonzero_padding} {
        if {$padding_length == 0} { error "test payload has no padding" }
        set padding [string replace $padding end end \x01]
    }
    set length [expr {8 + [string length $payload] + $padding_length}]
    return "[binary format I $length][binary format S 5][binary format S 0]$payload$padding"
}

proc compact_gossip_test_packet {extensions extension_count {legacy_count 0} {advertise 1}} {
    set sender_port [srv -1 port]
    set sender_cport [expr {$sender_port + 10000}]
    set legacy_gossip [string repeat \x00 [expr {104 * $legacy_count}]]
    set length [expr {2256 + [string length $legacy_gossip] + [string length $extensions]}]
    set header [build_cluster_bus_header [R 1 CLUSTER MYID] $sender_port \
        $sender_cport 0 $length $extension_count 0 4]
    # count is at offset 14; mflags[1] is at offset 2254.
    set header [string replace $header 14 15 [binary format S $legacy_count]]
    if {$advertise} {
        set header [string replace $header 2254 2254 [binary format c 1]]
    }
    return "$header$legacy_gossip$extensions"
}

proc compact_gossip_test_send {packet} {
    set host [srv 0 host]
    set bus_port [expr {[srv 0 port] + 10000}]
    if {$::tls} {
        set fd [::tls::socket \
            -cafile "$::tlsdir/ca.crt" \
            -certfile "$::tlsdir/client.crt" \
            -keyfile "$::tlsdir/client.key" \
            $host $bus_port]
    } else {
        set fd [socket $host $bus_port]
    }
    fconfigure $fd -translation binary -buffering full
    puts -nonewline $fd $packet
    flush $fd
    close $fd
}

start_cluster 2 0 {tags {external:skip cluster}} {
    test "Malformed compact gossip extensions are rejected" {
        set node_id [R 1 CLUSTER MYID]
        set id_bytes [binary format H* $node_id]
        set port [srv -1 port]
        set bus_port [expr {$port + 10000}]

        set entry [binary format c 0]
        append entry $id_bytes [binary format II 0 0]
        append entry [binary format c 9] "127.0.0.1"
        append entry [binary format SSSSS $port $bus_port 0 0 0]
        set valid_payload "[binary format S 1]$entry"
        set valid_ext [compact_gossip_test_extension $valid_payload]

        # A short entry is invalid even if the enclosing extension is padded.
        set short_payload "[binary format S 1][binary format c 0][string range $id_bytes 0 4]"
        set short_ext [compact_gossip_test_extension $short_payload]

        set oversized_ip_entry [binary format c 0]
        append oversized_ip_entry $id_bytes [binary format II 0 0]
        append oversized_ip_entry [binary format c 47] [string repeat x 47]
        append oversized_ip_entry [binary format SSSSS $port $bus_port 0 0 0]
        set oversized_ip_ext [compact_gossip_test_extension \
            "[binary format S 1]$oversized_ip_entry"]

        set cases [list \
            [list "truncated entry" $short_ext 1 0 1 "*Received invalid compact gossip extension*"] \
            [list "duplicate compact extension" "$valid_ext$valid_ext" 2 0 1 "*Received invalid compact gossip extension*"] \
            [list "legacy count and compact extension" $valid_ext 1 1 1 "*Received invalid compact gossip extension*"] \
            [list "IP length exceeds field" $oversized_ip_ext 1 0 1 "*Received invalid compact gossip extension*"] \
            [list "nonzero padding" [compact_gossip_test_extension $valid_payload 1] 1 0 1 "*Received invalid compact gossip extension*"] \
            [list "missing capability bit" $valid_ext 1 0 0 "*Received invalid compact gossip extension*"] \
            [list "extension length exceeds packet" \
                "[binary format I 64][binary format S 5][binary format S 0][binary format S 1]" \
                1 0 1 "*extension data that exceeds total packet length*"]]

        foreach case $cases {
            lassign $case name extensions extension_count legacy_count advertise log_pattern
            set log_line [count_log_lines 0]
            compact_gossip_test_send [compact_gossip_test_packet \
                $extensions $extension_count $legacy_count $advertise]
            wait_for_log_messages 0 [list $log_pattern] $log_line 50 100
            assert_equal PONG [R 0 PING]
            assert_equal ok [CI 0 cluster_state]
            assert_equal 2 [CI 0 cluster_known_nodes]
        }
    }

    test "Compact gossip accepts a raw node ID outside hexadecimal range" {
        set raw_id [string repeat g 40]
        set raw_entry [binary format c 1]
        append raw_entry $raw_id [binary format II 0 0]
        append raw_entry [binary format c 9] "127.0.0.1"
        append raw_entry [binary format SSSSS 65534 65535 0 0 0]
        set payload "[binary format S 1]$raw_entry"
        set extension [compact_gossip_test_extension $payload]

        set packet [compact_gossip_test_packet $extension 1]
        # Do not advertise synthetic slot or epoch changes for the sender.
        set packet [string replace $packet 16 31 [binary format WW 0 0]]
        compact_gossip_test_send $packet
        wait_for_condition 50 100 {
            [cluster_get_node_by_id 0 $raw_id] ne {}
        } else {
            fail "raw compact gossip node ID was not discovered"
        }
        assert_equal PONG [R 0 PING]
        assert_equal ok [CI 0 cluster_state]
    }
}

start_cluster 3 1 {tags {external:skip cluster}} {
    test "A rejoined spare node is discovered by every peer" {
        set old_id [R 4 CLUSTER MYID]
        isolate_node 4
        set new_id [R 4 CLUSTER MYID]
        assert {$new_id ne $old_id}
        wait_for_cluster_size 4

        set meet_port [srv -4 port]
        if {$::tls && ![lindex [R 0 CONFIG GET tls-cluster] 1]} {
            set meet_port [srv -4 pport]
        }
        R 0 CLUSTER MEET [srv -4 host] $meet_port
        wait_for_cluster_size 5
        wait_for_cluster_propagation
        wait_for_cluster_state ok

        for {set node 0} {$node < 4} {incr node} {
            assert {[cluster_get_node_by_id $node $new_id] ne {}}
            assert {[cluster_get_node_by_id $node $old_id] eq {}}
        }
    }

    test "Replica failure reports propagate to all masters and clear on recovery" {
        set replica_id [R 3 CLUSTER MYID]
        set replica_pid [srv -3 pid]
        pause_process $replica_pid
        set failed [catch {
            for {set master 0} {$master < 3} {incr master} {
                wait_node_marked_fail $master $replica_id
            }
        } error_message]
        resume_process $replica_pid
        if {$failed} { error $error_message }

        wait_for_condition 1000 50 {
            ![check_cluster_node_mark fail 0 $replica_id] &&
            ![check_cluster_node_mark fail 1 $replica_id] &&
            ![check_cluster_node_mark fail 2 $replica_id]
        } else {
            fail "replica FAIL flag did not clear after recovery"
        }
        wait_for_cluster_state ok
    }
} continuous_slot_allocation default_replica_allocation 5
