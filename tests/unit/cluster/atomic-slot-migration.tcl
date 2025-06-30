proc migration_status {node_id slots_range field} {
    set status [R $node_id CLUSTER MIGRATION STATUS]

    # Iterate through each migration operation
    foreach operation $status {
        set slots_found ""
        set field_value ""

        # Parse the key-value pairs in the operation
        for {set i 0} {$i < [llength $operation]} {incr i 2} {
            set key [lindex $operation $i]
            set value [lindex $operation [expr $i + 1]]

            if {$key eq "slots_range"} {
                set slots_found $value
            } elseif {$key eq $field} {
                set field_value $value
            }
        }
        # Check if this operation matches the requested slots_range
        if {$slots_found eq $slots_range} {
            return $field_value
        }
    }
    # Return empty string if slots_range not found or field not found
    return ""
}

start_cluster 3 3 {tags {external:skip cluster}} {
    test "Test IMPORT input validation" {
        # Invalid slot range
        assert_error {*wrong number of arguments*} {R 0 CLUSTER MIGRATION IMPORT}
        assert_error {*wrong number of arguments*} {R 0 CLUSTER MIGRATION IMPORT 100}
        assert_error {*wrong number of arguments*} {R 0 CLUSTER MIGRATION IMPORT 100 200 300}
        assert_error {*greater than end slot number*} {R 0 CLUSTER MIGRATION IMPORT 200 100}
        assert_error {*out of range slot*} {R 0 CLUSTER MIGRATION IMPORT 17000 18000}
        assert_error {*out of range slot*} {R 0 CLUSTER MIGRATION IMPORT 14000 18000}
        assert_error {*out of range slot*} {R 0 CLUSTER MIGRATION IMPORT -1 0}
        assert_error {*out of range slot*} {R 0 CLUSTER MIGRATION IMPORT sd sd}

        assert_error {*already the owner of the slot*} {R 0 CLUSTER MIGRATION IMPORT 100 200}
    }

    test "Test IMPORT not allowed on replica" {
        assert_error {* not allowed on replica*} {R 4 CLUSTER MIGRATION IMPORT 100 200}
    }

    test "Test IMPORT not allowed during manual migration" {
        set dst_id [R 1 CLUSTER MYID]

        # Set a slot to IMPORTING
        R 0 CLUSTER SETSLOT 15000 IMPORTING $dst_id
        assert_error {*must be STABLE to start*slot migration*} {R 0 CLUSTER MIGRATION IMPORT 100 200}
        # Revert the change
        R 0 CLUSTER SETSLOT 15000 STABLE

        # Same test with setting a slot to MIGRATING
        R 0 CLUSTER SETSLOT 5000 MIGRATING $dst_id
        assert_error {*must be STABLE to start*slot migration*} {R 0 CLUSTER MIGRATION IMPORT 100 200}
        # Revert the change
        R 0 CLUSTER SETSLOT 5000 STABLE
    }

    test "Test IMPORT not allowed if the node is already the owner" {
        assert_error {*already the owner of the slot*} {R 0 CLUSTER MIGRATION IMPORT 100 100}
    }

    test "Test IMPORT not allowed for a slot without an owner" {
        # Slot will have no owner
        R 0 CLUSTER DELSLOTS 5000

        assert_error {*slot has no owner: 5000*} {R 0 CLUSTER MIGRATION IMPORT 5000 5000}

        # Revert the change
        R 0 CLUSTER ADDSLOTS 5000
    }

    test "Test IMPORT not allowed if slot ranges belong to different nodes" {
        assert_error {*slots belong to different source nodes*} {R 0 CLUSTER MIGRATION IMPORT 7000 15000}
        assert_error {*slots belong to different source nodes*} {R 0 CLUSTER MIGRATION IMPORT 7000 8000 14000 15000}
    }

    test "Test IMPORT not allowed if slot is given multiple times" {
        assert_error {*Slot*specified multiple times*} {R 0 CLUSTER MIGRATION IMPORT 7000 8000 8000 9000}
        assert_error {*Slot*specified multiple times*} {R 0 CLUSTER MIGRATION IMPORT 7000 8000 7900 9000}
    }

    test "Test IMPORT not allowed if there is an overlapping import" {
        R 0 CLUSTER MIGRATION IMPORT 7000 8000
        assert_error {*overlapping import exists*} {R 0 CLUSTER MIGRATION IMPORT 8000 9000}
        assert_error {*overlapping import exists*} {R 0 CLUSTER MIGRATION IMPORT 7500 8500}
        assert_error {*overlapping import exists*} {R 0 CLUSTER MIGRATION IMPORT 6000 7000}
        assert_error {*overlapping import exists*} {R 0 CLUSTER MIGRATION IMPORT 6500 7500}
        wait_for_condition 1000 50 {
            [string match {*done*} [migration_status 0 7000-8000 state]] &&
            [string match {*done*} [migration_status 1 7000-8000 state]]
        } else {
            fail "ASM task did not start"
        }
    }

    test "Simple slot migration" {
        set slot0_key "06S"
        R 0 set $slot0_key "a"
        set slot1_key "Qi"
        R 0 set $slot1_key "b"
        set slot101_key "1j2"
        R 0 set $slot101_key "c"
        # 3 keys cost 3s to save
        R 0 config set rdb-key-save-delay 1000000
    
        # migrate slot 0-100 to R 1
        assert_equal {OK} [R 1 CLUSTER MIGRATION IMPORT 0 100]
        # migration is start, and in accumulating buffer stage
        wait_for_condition 1000 50 {
            [string match {*send-bulk-and-stream*} [R 0 cluster migration status]] &&
            [string match {*accumulate-buffer*} [R 1 cluster migration status]]
        } else {
            fail "ASM task did not start"
        }

        # append 99 times during migration
        for {set i 0} {$i < 99} {incr i} {
            R 0 append $slot0_key "a"
            R 0 append $slot1_key "b"
            R 0 append $slot101_key "c"
        }

        # wait until migration of 0-100 successful
        wait_for_condition 1000 50 {
            [string match {*done*} [migration_status 0 0-100 state]] &&
            [string match {*done*} [migration_status 1 0-100 state]]
        } else {
            fail "ASM task did not start"
        }

        # the appended 99 times should also be migrated
        assert_equal [string repeat a 100] [R 1 get $slot0_key]
        assert_equal [string repeat b 100] [R 1 get $slot1_key]
        # the slave should also get the data
        after 100
        R 4 readonly
        assert_equal [string repeat a 100] [R 4 get $slot0_key]
        assert_equal [string repeat b 100] [R 4 get $slot1_key]

        # verify key that was not in the slot range is not migrated
        assert_equal [string repeat c 100] [R 0 get $slot101_key]
        # verify changes in replica
        R 3 readonly
        assert_equal [string repeat c 100] [R 3 get $slot101_key]
    }
}
