# Initial non-contiguous slot distribution for four shards.
set ::cluster_shards_slot0 [list 0 1000 1002 5459 5461 5461 10926 10926]
set ::cluster_shards_slot1 [list 5460 5460 5462 10922 10925 10925]
set ::cluster_shards_slot2 [list 10923 10924 10927 16383]
set ::cluster_shards_slot3 [list 1001 1001]

proc cluster_shards_split_slot_allocation {masters replicas} {
    for {set id 0} {$id < $masters} {incr id} {
        R $id cluster ADDSLOTSRANGE {*}[set ::cluster_shards_slot${id}]
    }
}

# Return node or shard information for node_id as seen by reference. Valid
# type values are "node" and "shard".
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
    return {}
}

# The legacy runner provided a pool of 20 servers. Only nodes 0-7 initially
# belong to the four shards; nodes 8-19 remain unassigned for later tests.
start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-allow-replica-migration no}} {

test "Cluster should start ok" {
    wait_for_cluster_state ok
}

test "Set cluster hostnames and verify they are propagated" {
    for {set id 0} {$id < $::cluster_master_nodes + $::cluster_replica_nodes} {incr id} {
        R $id config set cluster-announce-hostname "host-$id.com"
    }
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

    for {set reference 0} {$reference < $::cluster_master_nodes + $::cluster_replica_nodes} {incr reference} {
        for {set id 0} {$id < $::cluster_master_nodes + $::cluster_replica_nodes} {incr id} {
            set node_id [lindex $ids $id]
            set shard [cluster_shards_get_node_info $node_id $reference shard]
            set node [cluster_shards_get_node_info $node_id $reference node]
            assert_equal [lindex $slots $id] [dict get $shard slots]
            assert_equal "host-$id.com" [dict get $node hostname]
            assert_equal "127.0.0.1" [dict get $node ip]
            # The default preferred endpoint type is IP.
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
                # A replica may still be loading during propagation.
            }
        }
    }
}

test "Verify no slot shard" {
    set node_8_id [R 8 CLUSTER MYID]
    assert_equal {} [dict get [cluster_shards_get_node_info $node_8_id 8 shard] slots]
    assert_equal {} [dict get [cluster_shards_get_node_info $node_8_id 0 shard] slots]
}

set node_0_id [R 0 CLUSTER MYID]

test "Kill a node and tell the replica to immediately takeover" {
    cluster_kill_node 0
    R 4 cluster failover force
}

test "Verify health as fail for killed node" {
    wait_for_condition 50 100 {
        [dict get [cluster_shards_get_node_info $node_0_id 4 node] health] eq "fail"
    } else {
        fail "New primary never detected the node failure"
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
        fail "Old primary was not converted into a replica"
    }
}

test "Test the replica reports a loading state while it's loading" {
    set replica_cluster_id [R $replica_id CLUSTER MYID]
    wait_for_condition 50 1000 {
        [dict get [cluster_shards_get_node_info $replica_cluster_id $primary_id node] health] eq "online"
    } else {
        fail "Replica never transitioned to online"
    }

    # Force the next synchronization to be a full sync with observable load
    # time while cluster messages continue to be processed.
    R $primary_id debug populate 1000 key 1000
    R $primary_id config set repl-backlog-size 100
    R $replica_id config set key-load-delay 4000
    R $replica_id config set loading-process-events-interval-bytes 1024

    R $primary_id multi
    R $primary_id client kill type replica
    set value [string repeat A 1024]
    for {set j 0} {$j < 100} {incr j} {
        R $primary_id set "{ch3}$j" $value
    }
    R $primary_id exec

    wait_for_condition 50 1000 {
        [dict get [cluster_shards_get_node_info $replica_cluster_id $primary_id node] health] eq "loading"
    } else {
        fail "Replica never transitioned to loading"
    }

    # Both topology commands must remain available while data is loading.
    R $replica_id CLUSTER SHARDS
    R $replica_id CLUSTER SLOTS

    R $replica_id config set key-load-delay 0
    wait_for_condition 50 1000 {
        [dict get [cluster_shards_get_node_info $replica_cluster_id $primary_id node] health] eq "online"
    } else {
        fail "Replica never transitioned to online"
    }
    assert_equal online [dict get [cluster_shards_get_node_info $replica_cluster_id $replica_id node] health]
}

test "Regression test for a crash when calling SHARDS during handshake" {
    set id [R 19 CLUSTER MYID]
    R 19 CLUSTER RESET HARD
    for {set other 0} {$other < 19} {incr other} {
        R $other CLUSTER FORGET $id
    }
    R 19 cluster meet 127.0.0.1 [srv 0 port]
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
