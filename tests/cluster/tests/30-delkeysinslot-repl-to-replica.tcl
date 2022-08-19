# Check delKeysInSlot replicate to replica

source "../tests/includes/init-tests.tcl"

test "Create a 3 nodes cluster" {
    create_cluster 3 3
}

test "Cluster is up" {
    assert_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test 0
}

test "Instance #3 is a replica" {
    assert {[RI 3 role] eq {slave}}
}

test "Instance #3 synced with the master" {
    wait_for_condition 1000 50 {
        [RI 3 master_link_status] eq {up}
    } else {
        fail "Instance #3 master link status is not up"
    }
}

test "Instance #4 is a replica" {
    assert {[RI 4 role] eq {slave}}
}

test "Instance #4 synced with the master" {
    wait_for_condition 1000 50 {
        [RI 4 master_link_status] eq {up}
    } else {
        fail "Instance #4 master link status is not up"
    }
}

set randomkey [R 0 randomkey]
set randomkey_slot [R 0 cluster keyslot $randomkey]
set slot_keys_num [R 0 cluster countkeysinslot $randomkey_slot]

test "Instance #3 is a replica, have the 'randomkey'" {
    R 3 readonly
    assert_equal [R 3 exists $randomkey] "1"
    assert {$slot_keys_num > 0}
    assert_equal [R 3 cluster countkeysinslot $randomkey_slot] $slot_keys_num

    assert_equal [R 1 cluster countkeysinslot $randomkey_slot] "0"
    assert_equal [R 4 cluster countkeysinslot $randomkey_slot] "0"
}

test "set the slot other node, src-node will delete keys in the slot and replicate to replica" {
    set nodeid [R 1 cluster myid]

    R 1 cluster bumpepoch
    # force assinged $randomkey_slot to node redis-1
    assert_equal [R 1 cluster setslot $randomkey_slot node $nodeid] "OK"

    wait_for_cluster_propagation
    
    assert_equal [R 1 exists $randomkey] "0"
    assert_equal [R 1 cluster countkeysinslot $randomkey_slot] "0"
    R 4 readonly
    assert_equal [R 4 exists $randomkey] "0"
    assert_equal [R 4 cluster countkeysinslot $randomkey_slot] "0"

    # src master will delete keys in the slot
    wait_for_condition 50 100 {
        [R 0 cluster countkeysinslot $randomkey_slot] eq 0
    } else {
        fail "master:cluster countkeysinslot $randomkey_slot did not eq 0"
    }
    
    # src replica will delete keys in the slot
    wait_for_condition 50 100 {
        [R 3 cluster countkeysinslot $randomkey_slot] eq 0
    } else {
        fail "replica:cluster countkeysinslot $randomkey_slot did not eq 0"
    }
}


