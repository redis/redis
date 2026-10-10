# Exercise the topology and failure information carried by short PING/PONG
# frames. The mixed-version wire test checks that the frames are actually short.

proc short_heartbeat_slot_owner {observer slot} {
    foreach node [get_cluster_nodes $observer] {
        foreach range [dict get $node slots] {
            if {![regexp {^([0-9]+)(-([0-9]+))?$} $range unused first unused2 last]} {
                continue
            }
            if {$last eq ""} { set last $first }
            if {$slot >= $first && $slot <= $last} {
                return [dict get $node id]
            }
        }
    }
    return ""
}

proc short_heartbeat_test_socket {} {
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
    return $fd
}

proc short_heartbeat_test_extension {type payload} {
    set length [expr {((8 + [string length $payload] + 7) / 8) * 8}]
    set padding [string repeat \x00 [expr {$length - 8 - [string length $payload]}]]
    return "[binary format ISS $length $type 0]$payload$padding"
}

proc short_heartbeat_test_frame {full_header extensions extension_count} {
    # Version 2 deletes only the 2048-byte slot bitmap at offsets 80..2127.
    set frame [string replace $full_header 80 2127 ""]
    set frame [string replace $frame 8 9 [binary format S 2]]
    set frame [string replace $frame 166 167 [binary format S $extension_count]]
    set frame [string replace $frame 4 7 \
        [binary format I [expr {208 + [string length $extensions]}]]]
    append frame $extensions
    return $frame
}

proc short_heartbeat_test_wait_for_pongs {fd expected} {
    fconfigure $fd -blocking 0
    set bytes ""
    set pongs {}
    for {set attempt 0} {$attempt < 50} {incr attempt} {
        append bytes [read $fd]
        while {[string length $bytes] >= 8} {
            binary scan [string range $bytes 4 7] I length
            if {$length < 208 || $length > 1000000} {
                error "invalid reply length on forged Cluster bus link"
            }
            if {[string length $bytes] < $length} { break }
            binary scan [string range $bytes 12 13] S type
            if {$type == 1} { lappend pongs [string range $bytes 0 [expr {$length - 1}]] }
            set bytes [string range $bytes $length end]
        }
        if {[llength $pongs] >= $expected} { return $pongs }
        after 100
    }
    error "forged Cluster bus link received [llength $pongs] PONGs, expected $expected"
}

proc short_heartbeat_reply_request_index {packet} {
    binary scan [string range $packet 8 9] S version
    if {$version == 1} {
        set offset 2254
    } elseif {$version == 2} {
        set offset 206
    } else {
        error "unexpected Cluster bus reply version $version"
    }
    binary scan [string range $packet $offset $offset] c flags
    if {!($flags & 8)} { return -1 }
    set index_offset [expr {$offset - 10}]
    binary scan [string range $packet $index_offset [expr {$index_offset + 1}]] S index
    return $index
}

proc short_heartbeat_compact_health_extension {node_id ip port cport} {
    set entry "[binary format c 0][binary format H* $node_id]"
    append entry [binary format II 0 [clock seconds]]
    append entry [binary format c [string length $ip]] $ip
    append entry [binary format SSSSS $port $cport 2 0 0]
    return [short_heartbeat_test_extension 5 "[binary format S 1]$entry"]
}

start_cluster 3 1 {tags {external:skip cluster}} {
    test "Short heartbeats start after the cluster converges" {
        wait_for_condition 200 100 {
            [CI 0 cluster_stats_bus_short_messages_sent] > 0 &&
            [CI 1 cluster_stats_bus_short_messages_received] > 0
        } else {
            fail "new peers did not start exchanging short heartbeats"
        }
    }

    test "Malformed short frames cannot change cluster state" {
        # Node 4 is a spare master with no slots. Its true zero bitmap has a
        # CRC64 of zero, so a forged full PING can negotiate this test link
        # without changing slot ownership on node 0.
        set sender_id [R 4 CLUSTER MYID]
        set sender_port [srv -4 port]
        set sender_cport [expr {$sender_port + 10000}]
        set ids {}
        for {set node 0} {$node < 5} {incr node} {
            lappend ids [R $node CLUSTER MYID]
        }
        set digest_input [binary format S 5]
        foreach id [lsort $ids] { append digest_input $id }
        set digest [binary format H* [R 0 EVAL {return redis.sha1hex(ARGV[1])} 0 $digest_input]]
        assert_equal 20 [string length $digest]

        set full [build_cluster_bus_header $sender_id $sender_port \
            $sender_cport 0 2256 0 1 4]
        set full [string replace $full 16 31 [binary format WW 0 0]]
        set full [string replace $full 2216 2235 $digest]
        set full [string replace $full 2254 2254 [binary format c 3]]

        set base [clock seconds]
        set index [lsearch -exact [lsort $ids] [R 2 CLUSTER MYID]]
        assert {$index >= 0}
        set valid_payload "[binary format SSI 1 0 $base][binary format SS $index 0]"
        set valid_ext [short_heartbeat_test_extension 6 $valid_payload]
        set valid_short [short_heartbeat_test_frame $full $valid_ext 1]

        # A short header below 208 bytes must be rejected at the bus reader.
        set too_short [string range $valid_short 0 206]
        set too_short [string replace $too_short 4 7 [binary format I 207]]
        set log_line [count_log_lines 0]
        set fd [short_heartbeat_test_socket]
        puts -nonewline $fd $too_short
        flush $fd
        close $fd
        wait_for_log_messages 0 {"*Bad message length or signature received*"} \
            $log_line 50 100

        # The full packet associates an inbound link with the known sender.
        # A valid v2 frame on that same socket proves the subsequent negative
        # cases reach the negotiated v2 decoder, not the unnegotiated guard.
        set epoch [CI 0 cluster_current_epoch]
        set spare_pid [srv -4 pid]
        pause_process $spare_pid
        set fd ""
        set failed [catch {
            set fd [short_heartbeat_test_socket]
                puts -nonewline $fd "$full$valid_short"
                flush $fd
                # The two PONGs prove both the v1 preamble and v2 probe were
                # accepted on this connection. Other cluster traffic cannot
                # satisfy this assertion.
                short_heartbeat_test_wait_for_pongs $fd 2

                # With a bad membership digest, the decoder requests a full
                # refresh before applying the impossible epoch.
                set wrong_digest [string replace $valid_short 168 187 [string repeat \x00 20]]
                set wrong_digest [string replace $wrong_digest 16 23 \
                    [binary format W 9223372036854775807]]
                puts -nonewline $fd $wrong_digest
                flush $fd
                after 100
                assert_equal $epoch [CI 0 cluster_current_epoch]

                # An extension claiming two indexed entries but carrying none
                # must fail whole-packet validation before state updates.
                set truncated_ext [short_heartbeat_test_extension 6 [binary format SSI 2 0 $base]]
                set truncated [short_heartbeat_test_frame $full $truncated_ext 1]
                set truncated [string replace $truncated 16 23 \
                    [binary format W 9223372036854775807]]
                set log_line [count_log_lines 0]
                puts -nonewline $fd $truncated
                flush $fd
                wait_for_log_messages 0 {"*Received invalid short gossip extension*"} \
                    $log_line 50 100
                assert_equal $epoch [CI 0 cluster_current_epoch]

                # FORGOTTEN_NODE naming the packet's sender must be ignored;
                # deleting it during packet processing would dangle the link.
                set forget_payload "$sender_id[binary format W 60]"
                set forget_ext [short_heartbeat_test_extension 2 $forget_payload]
                set self_delete [short_heartbeat_test_frame $full "$valid_ext$forget_ext" 2]
                puts -nonewline $fd $self_delete
                flush $fd
                after 100
                assert_equal $epoch [CI 0 cluster_current_epoch]
                assert {[cluster_get_node_by_id 0 $sender_id] ne {}}
                assert_equal 5 [CI 0 cluster_known_nodes]
                assert_equal PONG [R 0 PING]
        } error_message]
        if {$fd ne ""} { catch {close $fd} }
        resume_process $spare_pid
        if {$failed} { error $error_message }
        wait_for_cluster_state ok
    }

    test "Two slot transfers at one config epoch both propagate" {
        set destination_id [R 1 CLUSTER MYID]
        set first_slot 0
        set second_slot 1
        assert_equal [R 0 CLUSTER MYID] [short_heartbeat_slot_owner 2 $first_slot]
        assert_equal [R 0 CLUSTER MYID] [short_heartbeat_slot_owner 2 $second_slot]

        R 1 CLUSTER BUMPEPOCH
        set epoch [CI 1 cluster_my_epoch]
        assert_equal OK [R 1 CLUSTER SETSLOT $first_slot NODE $destination_id]
        wait_for_condition 200 100 {
            [short_heartbeat_slot_owner 0 $first_slot] eq $destination_id &&
            [short_heartbeat_slot_owner 2 $first_slot] eq $destination_id &&
            [short_heartbeat_slot_owner 3 $first_slot] eq $destination_id
        } else {
            fail "first slot transfer did not propagate"
        }
        wait_for_cluster_propagation

        # The second bitmap update must not depend on an epoch increase.
        assert_equal OK [R 1 CLUSTER SETSLOT $second_slot NODE $destination_id]
        assert_equal $epoch [CI 1 cluster_my_epoch]
        wait_for_condition 200 100 {
            [short_heartbeat_slot_owner 0 $second_slot] eq $destination_id &&
            [short_heartbeat_slot_owner 2 $second_slot] eq $destination_id &&
            [short_heartbeat_slot_owner 3 $second_slot] eq $destination_id
        } else {
            fail "second slot transfer at the same epoch did not propagate"
        }
        wait_for_cluster_propagation
        wait_for_cluster_state ok
    }

    test "Announced endpoint changes without an epoch change propagate" {
        set epoch [CI 0 cluster_my_epoch]
        R 0 CONFIG SET cluster-announce-hostname short-heartbeat.example
        wait_for_condition 200 100 {
            [string match "*short-heartbeat.example*" [R 1 CLUSTER NODES]] &&
            [string match "*short-heartbeat.example*" [R 2 CLUSTER NODES]]
        } else {
            fail "new announced hostname did not propagate"
        }
        assert_equal $epoch [CI 0 cluster_my_epoch]
        R 0 CONFIG SET cluster-announce-hostname ""
        wait_for_condition 200 100 {
            ![string match "*short-heartbeat.example*" [R 1 CLUSTER NODES]] &&
            ![string match "*short-heartbeat.example*" [R 2 CLUSTER NODES]]
        } else {
            fail "cleared announced hostname did not propagate"
        }
    }

    test "Membership change rebuilds the index and short frames resume" {
        set old_id [R 4 CLUSTER MYID]
        isolate_node 4
        set new_id [R 4 CLUSTER MYID]
        assert {$old_id ne $new_id}
        wait_for_cluster_size 4

        set meet_port [srv -4 port]
        if {$::tls && ![lindex [R 0 CONFIG GET tls-cluster] 1]} {
            set meet_port [srv -4 pport]
        }
        R 0 CLUSTER MEET [srv -4 host] $meet_port
        wait_for_cluster_size 5
        wait_for_cluster_propagation
        for {set node 0} {$node < 5} {incr node} {
            assert {[cluster_get_node_by_id $node $new_id] ne {}}
            assert {[cluster_get_node_by_id $node $old_id] eq {}}
        }

        set before [CI 4 cluster_stats_bus_short_messages_sent]
        wait_for_condition 200 100 {
            [CI 4 cluster_stats_bus_short_messages_sent] > $before
        } else {
            fail "short heartbeats did not resume after membership changed"
        }
        wait_for_cluster_state ok
    }

    test "Failure reports survive short heartbeats and clear after recovery" {
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
            fail "FAIL flag did not clear after replica recovery"
        }
        wait_for_cluster_state ok
    }

    test "Healthy indexed gossip requests and repairs a failed node address" {
        set target_id [R 3 CLUSTER MYID]
        set target_port [srv -3 port]
        set target_cport [expr {$target_port + 10000}]
        set sender_id [R 4 CLUSTER MYID]
        set sender_port [srv -4 port]
        set sender_cport [expr {$sender_port + 10000}]
        set ids {}
        for {set node 0} {$node < 5} {incr node} {
            lappend ids [R $node CLUSTER MYID]
        }
        set sorted_ids [lsort $ids]
        set index [lsearch -exact $sorted_ids $target_id]
        assert {$index >= 0}
        set digest_input [binary format S 5]
        foreach id $sorted_ids { append digest_input $id }
        set digest [binary format H* [R 0 EVAL {return redis.sha1hex(ARGV[1])} 0 $digest_input]]

        # A spare sender has an all-zero slot bitmap and CRC64. Keep the
        # members' real IDs, addresses and roles while forging bus packets.
        set full [build_cluster_bus_header $sender_id $sender_port \
            $sender_cport 0 2256 0 1 4]
        set full [string replace $full 16 31 [binary format WW 0 0]]
        set full [string replace $full 2216 2235 $digest]
        set full [string replace $full 2254 2254 [binary format c 3]]
        set payload "[binary format SSI 1 0 [clock seconds]][binary format SS $index 0]"
        set indexed_ext [short_heartbeat_test_extension 6 $payload]
        set indexed [short_heartbeat_test_frame $full $indexed_ext 1]
        set wrong_ext [short_heartbeat_compact_health_extension \
            $target_id 127.0.0.2 $target_port $target_cport]
        set wrong_address [short_heartbeat_test_frame $full $wrong_ext 1]

        set target_pid [srv -3 pid]
        set sender_pid [srv -4 pid]
        set target_paused 0
        set sender_paused 0
        set fd ""
        set failed [catch {
            pause_process $target_pid
            set target_paused 1
            for {set master 0} {$master < 3} {incr master} {
                wait_node_marked_fail $master $target_id
            }
            pause_process $sender_pid
            set sender_paused 1

            set fd [short_heartbeat_test_socket]
            puts -nonewline $fd "$full$wrong_address"
            flush $fd
            short_heartbeat_test_wait_for_pongs $fd 2

            wait_for_condition 50 100 {
                [string match "127.0.0.2:*" \
                    [dict get [cluster_get_node_by_id 0 $target_id] addr]]
            } else {
                fail "complete gossip did not update the failed node's stale address"
            }

            # Type6 carries no address. On a failed node it must request a
            # targeted full record and leave the stale address untouched.
            puts -nonewline $fd $indexed
            flush $fd
            set replies [short_heartbeat_test_wait_for_pongs $fd 1]
            assert_equal $index [short_heartbeat_reply_request_index [lindex $replies 0]]
            assert {[string match "127.0.0.2:*" \
                [dict get [cluster_get_node_by_id 0 $target_id] addr]]}

            set correct_ext [short_heartbeat_compact_health_extension \
                $target_id 127.0.0.1 $target_port $target_cport]
            puts -nonewline $fd [short_heartbeat_test_frame $full $correct_ext 1]
            flush $fd
            wait_for_condition 50 100 {
                [string match "127.0.0.1:*" \
                    [dict get [cluster_get_node_by_id 0 $target_id] addr]]
            } else {
                fail "complete gossip did not repair the failed node's address"
            }
        } error_message]
        if {$fd ne ""} { catch {close $fd} }
        if {$sender_paused} { resume_process $sender_pid }
        if {$target_paused} { resume_process $target_pid }
        if {$failed} { error $error_message }
        wait_for_condition 1000 50 {
            ![check_cluster_node_mark fail 0 $target_id]
        } else {
            fail "repaired node did not recover after resuming"
        }
        wait_for_cluster_state ok
    } {} {tls:skip}
} continuous_slot_allocation default_replica_allocation 5
