# Initial slot distribution.
set ::cluster_shards_slot0 [list 0 1000 1002 5459 5461 5461 10926 10926]
set ::cluster_shards_slot1 [list 5460 5460 5462 10922 10925 10925]
set ::cluster_shards_slot2 [list 10923 10924 10927 16383]
set ::cluster_shards_slot3 [list 1001 1001]

proc cluster_shards_split_slot_allocation {masters replicas} {
    for {set id 0} {$id < $masters} {incr id} {
        R $id cluster ADDSLOTSRANGE {*}[set ::cluster_shards_slot${id}]
    }
}

# Get the node info with the specific node_id from the
# given reference node. Valid type options are "node" and "shard"
proc cluster_shards_get_node_info {node_id reference {type node}} {
    foreach shard [R $reference CLUSTER SHARDS] {
        foreach node [dict get $shard nodes] {
            if {[dict get $node id] ne $node_id} continue
            if {$type eq "node"} {
                return $node
            } elseif {$type eq "shard"} {
                return $shard
            }
            return {}
        }
    }
    # No shard found, return nothing
    return {}
}

# The legacy runner provided a pool of 20 servers. Only nodes 0-7 initially
# belong to the four shards; nodes 8-19 remain unassigned for later tests.
start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-allow-replica-migration no}} {

test "Set cluster hostnames and verify they are propagated" {
    for {set id 0} {$id < $::cluster_master_nodes + $::cluster_replica_nodes} {incr id} {
        R $id config set cluster-announce-hostname "host-$id.com"
    }
    # Wait for everyone to agree about the state
    wait_for_cluster_propagation
}

test "Verify information about the shards" {
    set ids {}
    for {set id 0} {$id < $::cluster_master_nodes + $::cluster_replica_nodes} {incr id} {
        lappend ids [R $id CLUSTER MYID]
    }
    set slots [list \
        $::cluster_shards_slot0 $::cluster_shards_slot1 \
        $::cluster_shards_slot2 $::cluster_shards_slot3 \
        $::cluster_shards_slot0 $::cluster_shards_slot1 \
        $::cluster_shards_slot2 $::cluster_shards_slot3]

    # Verify on each node (primary/replica), the response of the `CLUSTER SLOTS` command is consistent.
    for {set reference 0} {$reference < $::cluster_master_nodes + $::cluster_replica_nodes} {incr reference} {
        for {set id 0} {$id < $::cluster_master_nodes + $::cluster_replica_nodes} {incr id} {
            set node_id [lindex $ids $id]
            set shard [cluster_shards_get_node_info $node_id $reference shard]
            set node [cluster_shards_get_node_info $node_id $reference node]
            assert_equal [lindex $slots $id] [dict get $shard slots]
            assert_equal "host-$id.com" [dict get $node hostname]
            assert_equal "127.0.0.1" [dict get $node ip]
            # Default value of 'cluster-preferred-endpoint-type' is ip.
            assert_equal "127.0.0.1" [dict get $node endpoint]

            if {$::tls} {
                assert_equal [srv -$id pport] [dict get $node port]
                assert_equal [srv -$id port] [dict get $node tls-port]
            } else {
                assert_equal [srv -$id port] [dict get $node port]
            }

            if {$id < 4} {
                assert_equal master [dict get $node role]
                assert_equal online [dict get $node health]
            } else {
                assert_equal replica [dict get $node role]
                # Replica could be in online or loading
            }
        }
    }
}

test "Verify no slot shard" {
    # Node 8 has no slots assigned
    set node_8_id [R 8 CLUSTER MYID]
    assert_equal {} [dict get [cluster_shards_get_node_info $node_8_id 8 shard] slots]
    assert_equal {} [dict get [cluster_shards_get_node_info $node_8_id 0 shard] slots]
}

set node_0_id [R 0 CLUSTER MYID]

test "Kill a node and tell the replica to immediately takeover" {
    cluster_kill_node 0
    R 4 cluster failover force
}

# Primary 0 node should report as fail, wait until the new primary acknowledges it.
test "Verify health as fail for killed node" {
    wait_for_condition 50 100 {
        [dict get [cluster_shards_get_node_info $node_0_id 4 node] health] eq "fail"
    } else {
        fail "New primary never detected the node failed"
    }
}

test "Verify that other nodes can correctly output the new master's slots" {
    set new_primary_id [R 4 CLUSTER MYID]
    assert_not_equal {} [dict get [cluster_shards_get_node_info $new_primary_id 8 shard] slots]
}

set primary_id 4
set replica_id 0

test "Restarting primary node" {
    cluster_restart_node $replica_id
}

test "Instance #0 gets converted into a replica" {
    wait_for_condition 1000 50 {
        [s -$replica_id role] eq {slave} &&
        [s -$replica_id master_link_status] eq {up}
    } else {
        fail "Old primary was not converted into replica"
    }
}

test "Test the replica reports a loading state while it's loading" {
    # Test the command is good for verifying everything moves to a happy state
    set replica_cluster_id [R $replica_id CLUSTER MYID]
    wait_for_condition 50 1000 {
        [dict get [cluster_shards_get_node_info $replica_cluster_id $primary_id node] health] eq "online"
    } else {
        fail "Replica never transitioned to online"
    }

    # Set 1 MB of data, so there is something to load on full sync
    R $primary_id debug populate 1000 key 1000

    # Kill replica client for primary and load new data to the primary
    R $primary_id config set repl-backlog-size 100

    # Set the key load delay so that it will take at least
    # 2 seconds to fully load the data.
    R $replica_id config set key-load-delay 4000

    # Trigger event loop processing every 1024 bytes, this trigger
    # allows us to send and receive cluster messages, so we are setting
    # it low so that the cluster messages are sent more frequently.
    R $replica_id config set loading-process-events-interval-bytes 1024

    R $primary_id multi
    R $primary_id client kill type replica
    # populate the correct data
    set value [string repeat A 1024]
    for {set j 0} {$j < 100} {incr j} {
        # Use hashtag valid for shard #0
        R $primary_id set "{ch3}$j" $value
    }
    R $primary_id exec

    # The replica should reconnect and start a full sync, it will gossip about it's health to the primary.
    wait_for_condition 50 1000 {
        [dict get [cluster_shards_get_node_info $replica_cluster_id $primary_id node] health] eq "loading"
    } else {
        fail "Replica never transitioned to loading"
    }

    # Verify cluster shards and cluster slots (deprecated) API responds while the node is loading data.
    R $replica_id CLUSTER SHARDS
    R $replica_id CLUSTER SLOTS

    # Speed up the key loading and verify everything resumes
    R $replica_id config set key-load-delay 0
    wait_for_condition 50 1000 {
        [dict get [cluster_shards_get_node_info $replica_cluster_id $primary_id node] health] eq "online"
    } else {
        fail "Replica never transitioned to online"
    }
    # Final sanity, the replica agrees it is online.
    assert_equal online [dict get [cluster_shards_get_node_info $replica_cluster_id $replica_id node] health]
}

test "Regression test for a crash when calling SHARDS during handshake" {
    # Reset forget a node, so we can use it to establish handshaking connections
    set id [R 19 CLUSTER MYID]
    R 19 CLUSTER RESET HARD
    for {set other 0} {$other < 19} {incr other} {
        R $other CLUSTER FORGET $id
    }
    R 19 cluster meet 127.0.0.1 [srv 0 port]
    # This should line would previously crash, since all the outbound
    # connections were in handshake state.
    R 19 CLUSTER SHARDS
}

test "Cluster is up" {
    wait_for_cluster_state ok
}

test "Shard ids are unique" {
    set shard_ids {}
    for {set id 0} {$id < 4} {incr id} {
        set shard_id [R $id cluster myshardid]
        assert_equal 0 [dict exists $shard_ids $shard_id]
        dict set shard_ids $shard_id 1
    }
}

test "CLUSTER MYSHARDID reports same id for both primary and replica" {
    for {set id 0} {$id < 4} {incr id} {
        assert_equal [R $id cluster myshardid] [R [expr {$id + 4}] cluster myshardid]
        assert_equal 40 [string length [R $id cluster myshardid]]
    }
}

test "New replica receives primary's shard id" {
    #find a primary
    set primary 0
    for {} {$primary < 8} {incr primary} {
        if {[regexp master [R $primary role]]} break
    }
    assert_not_equal [R 8 cluster myshardid] [R $primary cluster myshardid]
    assert_equal OK [R 8 cluster replicate [R $primary cluster myid]]
    assert_equal [R 8 cluster myshardid] [R $primary cluster myshardid]
}

test "CLUSTER MYSHARDID reports same shard id after shard restart" {
    set shard_ids {}
    for {set id 0} {$id < 8} {incr id 4} {
        dict set shard_ids $id [R $id cluster myshardid]
        cluster_kill_node $id
        assert {![is_alive [srv -$id pid]]}
    }
    for {set id 0} {$id < 8} {incr id 4} {
        cluster_restart_node $id
    }
    wait_for_cluster_state ok
    for {set id 0} {$id < 8} {incr id 4} {
        assert_equal [dict get $shard_ids $id] [R $id cluster myshardid]
    }
}

test "CLUSTER MYSHARDID reports same shard id after cluster restart" {
    set shard_ids {}
    for {set id 0} {$id < 8} {incr id} {
        dict set shard_ids $id [R $id cluster myshardid]
    }
    for {set id 0} {$id < 8} {incr id} {
        cluster_kill_node $id
        assert {![is_alive [srv -$id pid]]}
    }
    for {set id 0} {$id < 8} {incr id} {
        cluster_restart_node $id
    }
    wait_for_cluster_state ok
    for {set id 0} {$id < 8} {incr id} {
        assert_equal [dict get $shard_ids $id] [R $id cluster myshardid]
    }
}

} cluster_shards_split_slot_allocation default_replica_allocation 20 ;# start_cluster
