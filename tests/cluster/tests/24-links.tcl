source "../tests/includes/init-tests.tcl"

test "Create a cluster with two single-node shards" {
    create_cluster 2 0
}

test "Cluster should start ok" {
    assert_cluster_state ok
}

proc number_of_peers {id} {
    expr [llength [get_cluster_nodes $id]] - 1
}

proc number_of_links {id} {
    llength [get_cluster_links $id]
}

test "Each node has two links with each peer" {
    foreach_redis_id id {
        # Assert that from point of view of each node, there are two links for
        # each peer. It might take a while for cluster to stabilize so wait up
        # to 5 seconds.
        wait_for_condition 50 100 {
            [number_of_peers $id]*2 == [number_of_links $id]
        } else {
            assert_equal [expr [number_of_peers $id]*2] [number_of_links $id]
        }

        set nodes [get_cluster_nodes $id]
        set links [get_cluster_links $id]

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

    # On primary1, set cluster link send buffer limit to 256KB, which is large enough to not be
    # overflowed by regular gossip messages but also small enough that it doesn't take too much
    # memory to overflow it. If it is set too high, Redis may get OOM killed by kernel before this
    # limit is overflowed in some RAM-limited test environments.
    set oldlimit [lindex [$primary1 CONFIG get cluster-link-sendbuf-limit] 1]
    $primary1 CONFIG set cluster-link-sendbuf-limit [expr 256*1024]
    assert {[get_info_field [$primary1 cluster info] total_cluster_links_buffer_limit_exceeded] eq 0}

    # To manufacture an ever-growing send buffer from primary1 to primary2,
    # make primary2 unresponsive.
    set primary2_pid [get_instance_attrib redis $primary2_id pid]
    exec kill -SIGSTOP $primary2_pid

    # On primary1, send 128KB Pubsub messages in a loop until the send buffer of the link from
    # primary1 to primary2 exceeds buffer limit therefore be dropped.
    # For the send buffer to grow, we need to first exhaust TCP send buffer of primary1 and TCP
    # receive buffer of primary2 first. The sizes of these two buffers vary by OS, but 100 128KB
    # messages should be sufficient.
    set i 0
    wait_for_condition 100 0 {
        [catch {incr i} e] == 0 &&
        [catch {$primary1 publish channel [prepare_value [expr 128*1024]]} e] == 0 &&
        [catch {after 500} e] == 0 &&
        [get_info_field [$primary1 cluster info] total_cluster_links_buffer_limit_exceeded] >= 1
    } else {
        fail "Cluster link not freed as expected"
    }
    puts -nonewline "$i 128KB messages needed to overflow 256KB buffer limit. "

    # A new link to primary2 should have been recreated
    set new_link_p1_to_p2 [get_link_to_peer $primary1_id $primary2_name]
    assert {[dict get $new_link_p1_to_p2 create_time] > [dict get $orig_link_p1_to_p2 create_time]}

    # Link from primary2 should not be affected
    set same_link_p1_from_p2 [get_link_from_peer $primary1_id $primary2_name]
    assert {[dict get $same_link_p1_from_p2 create_time] eq [dict get $orig_link_p1_from_p2 create_time]}

    # Revive primary2
    exec kill -SIGCONT $primary2_pid

    # Reset configs on primary1 so config changes don't leak out to other tests
    $primary1 CONFIG set cluster-node-timeout $oldtimeout
    $primary1 CONFIG set cluster-link-sendbuf-limit $oldlimit
}
