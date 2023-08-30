start_cluster 2 2 {tags {external:skip cluster}} {

    test "Verify that write command works on replica with replica-read-only set to false cluster-enabled setup" {
        assert {[s -2 role] eq {slave}}
        wait_for_condition 1000 50 {
            [s -2 master_link_status] eq {up}
        } else {
            fail "Instance #2 master link status is not up"
        }

        assert {[s -3 role] eq {slave}}
        wait_for_condition 1000 50 {
            [s -3 master_link_status] eq {up}
        } else {
            fail "Instance #3 master link status is not up"
        }

        # Set a single key that will be used to test deletion
        set key "FOO"
        R 0 SET $key TEST
        set key_slot [R 0 cluster keyslot $key]
        set slot_keys_num [R 0 cluster countkeysinslot $key_slot]
        assert {$slot_keys_num > 0}

        R 2 readonly
        # Wait for replica to have the key
        wait_for_condition 1000 50 {
            [R 2 exists $key] eq "1"
        } else {
            fail "Test key was not replicated"
        }

        assert_equal [R 2 cluster countkeysinslot $key_slot] $slot_keys_num

        R 2 config set replica-read-only no
        R 2 DEL $key

        assert_equal [R 2 cluster countkeysinslot $key_slot] 0
        assert_equal [R 0 cluster countkeysinslot $key_slot] 1
    }

    test {Writable eplica changes gets overwritten by replication from primary} {
        assert_equal [R 2 cluster countkeysinslot $key_slot] 0
        R 0 SET $key bar

        assert_equal [R 0 cluster countkeysinslot $key_slot] 1
        wait_for_condition 1000 50 {
            [R 2 cluster countkeysinslot $key_slot] eq 1
        } else {
            fail "Test key was not replicated"
        }
        assert_equal [R 2 GET $key] bar
        R 0 DEL $key
        assert_equal [R 0 cluster countkeysinslot $key_slot] 0
        wait_for_condition 1000 50 {
            [R 2 cluster countkeysinslot $key_slot] eq 0
        } else {
            fail "Test key was not replicated"
        }
    }

    test {Replica is able to evict keys created in writable replicas} {
        set key "FOO"

        R 2 SET "{$key}1" 1 ex 5
        R 2 SET "{$key}2" 1 ex 5
        R 2 SET "{$key}3" 1 ex 5
        assert_equal [R 2 cluster countkeysinslot $key_slot] 3
        after 6000
        assert_equal [R 2 cluster countkeysinslot $key_slot] 0
    }

    test {Slot migration purges the keys created on the replicas} {
        R 0 SET "$key" 1
        assert_equal [R 0 cluster countkeysinslot $key_slot] 1

        # Wait for replica to have the key
        wait_for_condition 1000 50 {
            [R 2 cluster countkeysinslot $key_slot] eq 1
        } else {
            fail "Test key was not replicated"
        }

        R 2 SET "{$key}1" 1
        assert_equal [R 2 cluster countkeysinslot $key_slot] 2

        R 1 cluster bumpepoch
        
        set tgtnode [R 1 cluster myid]
        # Move $key_slot to node 1
        assert_equal [R 1 cluster setslot $key_slot node $tgtnode] "OK"

        wait_for_cluster_propagation

        # src master will delete keys in the slot
        wait_for_condition 50 100 {
            [R 0 cluster countkeysinslot $key_slot] eq 0
        } else {
            fail "primary 'countkeysinslot $key_slot' did not eq 0"
        }

        # src replica will delete keys in the slot
        wait_for_condition 50 100 {
            [R 2 cluster countkeysinslot $key_slot] eq 0
        } else {
            fail "replica 'countkeysinslot $key_slot' did not eq 0"
        }
    }
}
