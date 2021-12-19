source "../tests/includes/init-tests.tcl"

test "Create a cluster with two single-node shards" {
    create_cluster 2 0
}

test "Cluster should start ok" {
    assert_cluster_state ok
}

test "Each node has two links with each peer" {
    foreach_redis_id id {
        # Get number of peers, excluding myself
        set nodes [get_cluster_nodes $id]
        set num_peers [expr [llength $nodes] - 1]

        # Get number of links to peers
        set links [get_cluster_links $id]
        set num_links [llength $links]

        # Two links per peer
        assert {$num_peers*2 eq $num_links}

        # For each peer there should be exactly one
        # link "to" it and one link "from" it.
        foreach n $nodes {
            if {[has_flag $n myself]} continue
            set peer [dict get $n id]
            set to 0
            set from 0
            foreach l $links {
                if {[dict get $l node] eq $peer} {
                    if {[dict get $l dir] eq "to"} {
                        incr to
                    } elseif {[dict get $l dir] eq "from"} {
                        incr from
                    }
                }
            }
            assert {$to eq 1}
            assert {$from eq 1}
        }
    }
}

set primary1_id 0
set primary2_id 1

set primary1 [Rn $primary1_id]
set primary2 [Rn $primary2_id]

test "Disconnect link when send buffer limit reached" {
    # On primary1, set timeout to 1 hour so links won't get disconnected due to timeouts
    set oldtimeout [lindex [$primary1 CONFIG get cluster-node-timeout] 1]
    $primary1 CONFIG set cluster-node-timeout [expr 60*60*1000]

    # Get primary1's links with primary2
    set primary2_name [dict get [get_myself $primary2_id] id]
    set orig_link_p1_to_p2 [get_link_to_peer $primary1_id $primary2_name]
    set orig_link_p1_from_p2 [get_link_from_peer $primary1_id $primary2_name]

    # On primary1, set cluster link send buffer limit to 32MB
    set oldlimit [lindex [$primary1 CONFIG get cluster-link-sendbuf-limit] 1]
    $primary1 CONFIG set cluster-link-sendbuf-limit [expr 32*1024*1024]
    assert {[get_info_field [$primary1 cluster info] total_cluster_links_buffer_limit_exceeded] eq 0}

    # To manufacture an ever-growing send buffer from primary1 to primary2,
    # make primary2 unresponsive.
    set primary2_pid [get_instance_attrib redis $primary2_id pid]
    exec kill -SIGSTOP $primary2_pid

    # On primary1, send a 10MB Pubsub message. It will stay in send buffer of
    # the link from primary1 to primary2
    $primary1 publish channel [prepare_value [expr 10*1024*1024]]

    # Check the same link has not been disconnected, but its send buffer has grown
    set same_link_p1_to_p2 [get_link_to_peer $primary1_id $primary2_name]
    assert {[dict get $same_link_p1_to_p2 create_time] eq [dict get $orig_link_p1_to_p2 create_time]}
    assert {[dict get $same_link_p1_to_p2 send_buffer_allocated] > [dict get $orig_link_p1_to_p2 send_buffer_allocated]}

    # On primary1, send another 30MB Pubsub message.
    $primary1 publish channel [prepare_value [expr 30*1024*1024]]

    # Link has exceeded buffer limit and been dropped and recreated
    set new_link_p1_to_p2 [get_link_to_peer $primary1_id $primary2_name]
    assert {[dict get $new_link_p1_to_p2 create_time] > [dict get $orig_link_p1_to_p2 create_time]}
    assert {[get_info_field [$primary1 cluster info] total_cluster_links_buffer_limit_exceeded] eq 1}

    # Link from primary2 should not be affected
    set same_link_p1_from_p2 [get_link_from_peer $primary1_id $primary2_name]
    assert {[dict get $same_link_p1_from_p2 create_time] eq [dict get $orig_link_p1_from_p2 create_time]}

    # Revive primary2
    exec kill -SIGCONT $primary2_pid

    # Reset configs on primary1 so config changes don't leak out to other tests
    $primary1 CONFIG set cluster-node-timeout $oldtimeout
    $primary1 CONFIG set cluster-link-sendbuf-limit $oldlimit
}
