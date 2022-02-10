source "../tests/includes/init-tests.tcl"

proc cluster_allocate_with_split_slots {n} {
    R 0 cluster ADDSLOTSRANGE 0 1000 1002 5459 5461 5461 10926 10926
    R 1 cluster ADDSLOTSRANGE 5460 5460 5462 10922 10925 10925
    R 2 cluster ADDSLOTSRANGE 10923 10924 10927 16383
    R 3 cluster ADDSLOTSRANGE 1001 1001
}

proc cluster_create_with_split_slots {masters replicas} {
    cluster_allocate_with_split_slots $masters
    if {$replicas} {
        cluster_allocate_slaves $masters $replicas
    }
    set ::cluster_master_nodes $masters
    set ::cluster_replica_nodes $replicas
    assert_cluster_state ok
}

test "Create a 4 nodes cluster" {
    cluster_create_with_split_slots 4 4
}

test "Cluster should start ok" {
    assert_cluster_state ok
}

proc dict_shard_cmp {actual expected} {
    set keys [dict keys $actual]
    foreach key $keys {
        if {$key == "replication-offset"} {
            set health [dict get $actual health]
            if {$health == "ONLINE"} {
                assert {[dict get $actual $key] > 0}
            } else {
                assert {[dict get $actual $key] == 0}
            }
        } else {
            assert_equal [dict get $actual $key] [dict get $expected $key]
        }
    }
}

test "Set cluster hostnames and verify they are propagated" {
    for {set j 0} {$j < $::cluster_master_nodes + $::cluster_replica_nodes} {incr j} {
        R $j config set cluster-announce-hostname "host-$j.com"
    }

    wait_for_condition 50 100 {
        [are_hostnames_propagated "host-*.com"] eq 1
    } else {
        fail "cluster hostnames were not propagated"
    }

    # Now that everything is propagated, assert everyone agrees
    wait_for_cluster_propagation
}

# Fixed values
set primary [dict create id id port port ip 127.0.0.1 role primary health ONLINE]
set replica [dict create id id port port ip 127.0.0.1 role replica health ONLINE]

proc dict_setup_for_verification {node i} {
    upvar $node node_ref
    set port [expr 30000+$i]
    dict set node_ref id [R $i CLUSTER MYID]
    dict set node_ref port $port
    dict set node_ref hostname "host-$i.com"
}

# Initial slot distribution.
set slot1 [list 1001]
set slot2 [list 10923-10924 10927-16383]
set slot3 [list 5460 5462-10922 10925]
set slot4 [list 0-1000 1002-5459 5461 10926]

# Verify various combinations of `CLUSTER SHARDS` response
# 1. Single slot owner
# 2. Single slot(s) + multi slot range owner
# 3. multi slot range + multi slot range owner
test "Verify cluster shards response" {
    after 5000
    set shards [R 0 CLUSTER SHARDS]
    foreach shard $shards {
        set slots [dict get $shard slots]
        set nodes [dict get $shard nodes]
        if { $slots == $slot1 } {
            dict_setup_for_verification primary 3
            dict_setup_for_verification replica 7
            dict_shard_cmp [lindex $nodes 0] $primary
            dict_shard_cmp [lindex $nodes 1] $replica
        }
        if { $slots == $slot2 } {
            dict_setup_for_verification primary 2
            dict_setup_for_verification replica 6
            dict_shard_cmp [lindex $nodes 0] $primary
            dict_shard_cmp [lindex $nodes 1] $replica
        }
        if { $slots == $slot3 } {
            dict_setup_for_verification primary 1
            dict_setup_for_verification replica 5
            dict_shard_cmp [lindex $nodes 0] $primary
            dict_shard_cmp [lindex $nodes 1] $replica
        }
        if { $slots == $slot4 } {
            dict_setup_for_verification primary 0
            dict_setup_for_verification replica 4
            dict_shard_cmp [lindex $nodes 0] $primary
            dict_shard_cmp [lindex $nodes 1] $replica
       }
    }
}

# Remove the only slot owned by primary 3, slots array should be empty.
test "Verify no slots shard" {
    R 3 cluster DELSLOTSRANGE 1001 1001
    set shards [R 0 CLUSTER SHARDS]
    foreach shard $shards {
        set slots [dict get $shard slots]
        set nodes [dict get $shard nodes]
        if {[llength $slots] == 0 && [llength $nodes] == 2} {
                dict_setup_for_verification primary 3
                dict_setup_for_verification replica 7
                dict_shard_cmp [lindex $nodes 0] $primary
                dict_shard_cmp [lindex $nodes 1] $replica
        }
    }
}

set id0 [R 0 CLUSTER MYID]

test "Killing one master node" {
    kill_instance redis 0
}

test "Cluster should be down now" {
    assert_cluster_state fail
}

# Primary 0 node should report as fail.
test "Verify health as fail for killed node" {
    set shards [R 1 CLUSTER SHARDS]
    foreach shard $shards {
        set slots [dict get $shard slots]
        set nodes [dict get $shard nodes]
        if { $slots == $slot4 } {
            dict set primary id $id0
            dict set primary hostname "host-0.com"
            dict set primary port 30000
            dict set primary health FAIL
            dict_shard_cmp [lindex $nodes 0] $primary
        }
    }
}