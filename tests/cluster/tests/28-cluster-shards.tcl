source "../tests/includes/init-tests.tcl"

# Initial slot distribution.
set ::slot0 [list 0 1000 1002 5459 5461 5461 10926 10926]
set ::slot1 [list 5460 5460 5462 10922 10925 10925]
set ::slot2 [list 10923 10924 10927 16383]
set ::slot3 [list 1001 1001]

proc cluster_create_with_split_slots {masters replicas} {
    for {set j 0} {$j < $masters} {incr j} {
        R $j cluster ADDSLOTSRANGE {*}[set ::slot${j}]
    }
    if {$replicas} {
        cluster_allocate_slaves $masters $replicas
    }
    set ::cluster_master_nodes $masters
    set ::cluster_replica_nodes $replicas
}

# Get the node info with the specific node_id from the
# given reference node. Valid type options are "node" and "shard"
proc get_node_info_from_shard {id reference {type node}} {
    set shards_response [R $reference CLUSTER SHARDS]
    foreach shard_response $shards_response {
        set nodes [dict get $shard_response nodes]
        foreach node $nodes {
            if {[dict get $node id] eq $id} {
                if {$type eq "node"} {
                    return $node
                } elseif {$type eq "shard"} {
                    return $shard_response
                } else {
                    return {}
                }
            }
        }
    }
    # No shard found, return nothing
    return {}
}

test "Create a 8 nodes cluster with 4 shards" {
    cluster_create_with_split_slots 4 4
}

test "Cluster should start ok" {
    assert_cluster_state ok
}

test "Set cluster hostnames and verify they are propagated" {
    for {set j 0} {$j < $::cluster_master_nodes + $::cluster_replica_nodes} {incr j} {
        R $j config set cluster-announce-hostname "host-$j.com"
    }

    # Wait for everyone to agree about the state
    wait_for_cluster_propagation
}

test "Verify information about the shards" {
    set ids {}
    for {set j 0} {$j < $::cluster_master_nodes + $::cluster_replica_nodes} {incr j} {
        lappend ids [R $j CLUSTER MYID]
    }
    set slots [list $::slot0 $::slot1 $::slot2 $::slot3 $::slot0 $::slot1 $::slot2 $::slot3]

    # Verify on each node (primary/replica), the response of the `CLUSTER SLOTS` command is consistent.
    for {set ref 0} {$ref < $::cluster_master_nodes + $::cluster_replica_nodes} {incr ref} {
        for {set i 0} {$i < $::cluster_master_nodes + $::cluster_replica_nodes} {incr i} {
            assert_equal [lindex $slots $i] [dict get [get_node_info_from_shard [lindex $ids $i] $ref "shard"] slots]
            assert_equal "host-$i.com" [dict get [get_node_info_from_shard [lindex $ids $i] $ref "node"] hostname]
            assert_equal "127.0.0.1"  [dict get [get_node_info_from_shard [lindex $ids $i] $ref "node"] ip]
            # Default value of 'cluster-preferred-endpoint-type' is ip.
            assert_equal "127.0.0.1"  [dict get [get_node_info_from_shard [lindex $ids $i] $ref "node"] endpoint]

            if {$::tls} {
                assert_equal [get_instance_attrib redis $i plaintext-port] [dict get [get_node_info_from_shard [lindex $ids $i] $ref "node"] port]
                assert_equal [get_instance_attrib redis $i port] [dict get [get_node_info_from_shard [lindex $ids $i] $ref "node"] tls-port]
            } else {
                assert_equal [get_instance_attrib redis $i port] [dict get [get_node_info_from_shard [lindex $ids $i] $ref "node"] port]
            }

            if {$i < 4} {
                assert_equal "master" [dict get [get_node_info_from_shard [lindex $ids $i] $ref "node"] role]
                assert_equal "online" [dict get [get_node_info_from_shard [lindex $ids $i] $ref "node"] health]
            } else {
                assert_equal "replica" [dict get [get_node_info_from_shard [lindex $ids $i] $ref "node"] role]
                # Replica could be in online or loading
            }
        }
    }    
}

test "Verify no slot shard" {
    # Node 8 has no slots assigned
    set node_8_id [R 8 CLUSTER MYID]
    assert_equal {} [dict get [get_node_info_from_shard $node_8_id 8 "shard"] slots]
    assert_equal {} [dict get [get_node_info_from_shard $node_8_id 0 "shard"] slots]
}

set node_0_id [R 0 CLUSTER MYID]

test "Kill a node and tell the replica to immediately takeover" {
    kill_instance redis 0
    R 4 cluster failover force
}

# Primary 0 node should report as fail, wait until the new primary acknowledges it.
test "Verify health as fail for killed node" {
    wait_for_condition 50 100 {
        "fail" eq [dict get [get_node_info_from_shard $node_0_id 4 "node"] "health"]
    } else {
        fail "New primary never detected the node failed"
    }
}

set primary_id 4
set replica_id 0

test "Restarting primary node" {
    restart_instance redis $replica_id
}

test "Instance #0 gets converted into a replica" {
    wait_for_condition 1000 50 {
        [RI $replica_id role] eq {slave}
    } else {
        fail "Old primary was not converted into replica"
    }
}

test "Test the replica reports a loading state while it's loading" {
    # Test the command is good for verifying everything moves to a happy state
    set replica_cluster_id [R $replica_id CLUSTER MYID]
    wait_for_condition 50 1000 {
        [dict get [get_node_info_from_shard $replica_cluster_id $primary_id "node"] health] eq "online"
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
    set num 100
    set value [string repeat A 1024]
    for {set j 0} {$j < $num} {incr j} {
        # Use hashtag valid for shard #0
        set key "{ch3}$j"
        R $primary_id set $key $value
    }
    R $primary_id exec

    # The replica should reconnect and start a full sync, it will gossip about it's health to the primary.
    wait_for_condition 50 1000 {
        "loading" eq [dict get [get_node_info_from_shard $replica_cluster_id $primary_id "node"] health]
    } else {
        fail "Replica never transitioned to loading"
    }

    # Speed up the key loading and verify everything resumes
    R $replica_id config set key-load-delay 0

    wait_for_condition 50 1000 {
        "online" eq [dict get [get_node_info_from_shard $replica_cluster_id $primary_id "node"] health]
    } else {
        fail "Replica never transitioned to online"
    }

    # Final sanity, the replica agrees it is online. 
    assert_equal "online" [dict get [get_node_info_from_shard $replica_cluster_id $replica_id "node"] health]
}

test "Regression test for a crash when calling SHARDS during handshake" {
    # Reset forget a node, so we can use it to establish handshaking connections
    set id [R 19 CLUSTER MYID]
    R 19 CLUSTER RESET HARD
    for {set i 0} {$i < 19} {incr i} {
        R $i CLUSTER FORGET $id
    }
    R 19 cluster meet 127.0.0.1 [get_instance_attrib redis 0 port]
    # This should line would previously crash, since all the outbound
    # connections were in handshake state.
    R 19 CLUSTER SHARDS
}

test "Cluster is up" {
    assert_cluster_state ok
}
