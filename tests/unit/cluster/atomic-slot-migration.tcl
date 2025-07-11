proc migration_status {node_id task_id field} {
    set status [R $node_id CLUSTER MIGRATION STATUS]

    # Iterate through each migration operation
    foreach operation $status {
        set task_id_found ""
        set field_value ""

        # Parse the key-value pairs in the operation
        for {set i 0} {$i < [llength $operation]} {incr i 2} {
            set key [lindex $operation $i]
            set value [lindex $operation [expr $i + 1]]

            if {$key eq "id"} {
                set task_id_found $value
            } elseif {$key eq $field} {
                set field_value $value
            }
        }
        # Check if this operation matches the requested task_id
        if {$task_id_found eq $task_id} {
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
        # Let slot migration take long time, so that we can test overlapping import
        R 1 config set rdb-key-save-delay 1000000
        R 1 set tag22273 tag22273 ;# slot hash is 7000
        R 1 set tag9283 tag9283 ;# slot hash is 8000

        set task_id [R 0 CLUSTER MIGRATION IMPORT 7000 8000]
        assert_error {*overlapping import exists*} {R 0 CLUSTER MIGRATION IMPORT 8000 9000}
        assert_error {*overlapping import exists*} {R 0 CLUSTER MIGRATION IMPORT 7500 8500}
        assert_error {*overlapping import exists*} {R 0 CLUSTER MIGRATION IMPORT 6000 7000}
        assert_error {*overlapping import exists*} {R 0 CLUSTER MIGRATION IMPORT 6500 7500}

        wait_for_condition 1000 50 {
            [string match {*done*} [migration_status 0 $task_id state]] &&
            [string match {*done*} [migration_status 1 $task_id state]]
        } else {
            fail "ASM task did not start"
        }
        assert_equal "tag22273" [R 0 get tag22273]
        assert_equal "tag9283" [R 0 get tag9283]
        R 1 config set rdb-key-save-delay 0
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
        set task_id [R 1 CLUSTER MIGRATION IMPORT 0 100]
        # migration is start, and in accumulating buffer stage
        wait_for_condition 1000 50 {
            [string match {*send-bulk-and-stream*} [migration_status 0 $task_id state]] &&
            [string match {*accumulate-buffer*} [migration_status 1 $task_id state]]
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
            [string match {*done*} [migration_status 0 $task_id state]] &&
            [string match {*done*} [migration_status 1 $task_id state]]
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

        R 0 config set rdb-key-save-delay 0
    }

    proc asm_basic_error_handling_test {operation channel all_states} {
        foreach state $all_states {
            if {$::verbose} { puts "Testing $operation $channel channel with state: $state"}

            # For states that need incremental data streaming, set a longer delay
            set streaming_states [list "streaming-buffer" "accumulate-buffer" "send-bulk-and-stream"]
            if {$state in $streaming_states} {
                R 1 config set rdb-key-save-delay 1000000
            }

            # Start the slot 0 write load on the R 1
            if {$::tls} { set port [lindex [R 1 config get tls-port] 1]
            } else { set port [lindex [R 1 config get port] 1] }
            set load_handle [start_write_load "127.0.0.1" $port 500 "06S"]

            # clear old fail points and set the new fail point
            assert_equal {OK} [R 0 debug asm-failpoint "" ""]
            assert_equal {OK} [R 1 debug asm-failpoint "" ""]
            if {$operation eq "import"} {
                assert_equal {OK} [R 0 debug asm-failpoint "import-$channel-channel" $state]
            } elseif {$operation eq "migrate"} {
                assert_equal {OK} [R 1 debug asm-failpoint "migrate-$channel-channel" $state]
            } else {
                fail "Unknown operation: $operation"
            }

            # Start the migration
            set task_id [R 0 CLUSTER MIGRATION IMPORT 0 100]

            # The task should be failed due to the fail point
            wait_for_condition 1000 50 {
                [string match -nocase "*$channel*${state}*" [migration_status 0 $task_id last_error]] ||
                [string match -nocase "*$channel*${state}*" [migration_status 1 $task_id last_error]]
            } else {
                fail "ASM task did not fail with expected error -
                     (dst: [migration_status 0 $task_id last_error]
                      src: [migration_status 1 $task_id last_error]
                      expected: $channel $state)"
            }
            R 1 config set rdb-key-save-delay 0
            stop_write_load $load_handle

            # Cancel the task
            R 0 CLUSTER MIGRATION CANCEL ID $task_id
            R 1 CLUSTER MIGRATION CANCEL ID $task_id
        }
    }

    test "Destination node main channel basic error-handling tests " {
        set all_states [list \
            "connecting" \
            "auth-reply" \
            "handshake-reply" \
            "syncslots-reply" \
            "accumulate-buffer" \
            "streaming-buffer" \
            "wait-stream-eof" \
        ]
        asm_basic_error_handling_test "import" "main" $all_states
    }

    test "Destination node rdb channel basic error-handling tests" {
        set all_states [list \
            "connecting" \
            "auth-reply" \
            "rdbchannel-reply" \
            "rdbchannel-transfer" \
        ]
        asm_basic_error_handling_test "import" "rdb" $all_states
    }

    test "Source node main channel basic error-handling tests " {
        set all_states [list \
            "wait-rdbchannel" \
            "send-bulk-and-stream" \
            "handoff" \
        ]
        asm_basic_error_handling_test "migrate" "main" $all_states
    }

    test "Source node rdb channel basic error-handling tests" {
        set all_states [list \
            "wait-bgsave-start" \
            "send-bulk-and-stream" \
        ]
        asm_basic_error_handling_test "migrate" "rdb" $all_states
    }

    test "Migration will be successful after fail points are cleared" {
        # we set a delay to write incremental data
        R 1 config set rdb-key-save-delay 1000000

        # Start the slot 0 write load on the R 1
        if {$::tls} { set port [lindex [R 1 config get tls-port] 1]
        } else { set port [lindex [R 1 config get port] 1] }
        set load_handle [start_write_load "127.0.0.1" $port 100 "06S"]

        # Clear all fail points
        assert_equal {OK} [R 0 debug asm-failpoint "" ""]
        assert_equal {OK} [R 1 debug asm-failpoint "" ""]

        # Start the migration
        set task_id [R 0 CLUSTER MIGRATION IMPORT 0 100]

        # Wait for the migration to complete
        wait_for_condition 1000 50 {
            [string match {*done*} [migration_status 0 $task_id state]] &&
            [string match {*done*} [migration_status 1 $task_id state]]
        } else {
            fail "ASM task did not complete successfully"
        }

        stop_write_load $load_handle

        # Verify the data is migrated, slot 0 and 1 should belong to R 1
        # slot 0 key should be changed by the write load
        assert_not_equal [string repeat a 100] [R 0 get "06S"]
        assert_equal [string repeat b 100] [R 0 get "Qi"]
        # Slave should also get the data
        after 100
        R 3 readonly
        assert_equal [string repeat b 100] [R 3 get "Qi"]
        R 1 config set rdb-key-save-delay 0
    }

    test "client output buffer limit is reached on source side" {
        set r1_pid [getInfoProperty [R 1 info] process_id]
        R 1 debug repl-pause on-streaming-repl-buf

        # Set a small output buffer limit to trigger the error
        R 0 config set client-output-buffer-limit "replica 1024 0 0"
        # we set a delay to write incremental data
        R 0 config set rdb-key-save-delay 1000000

        set task_id [R 1 CLUSTER MIGRATION IMPORT 0 100]

        wait_for_condition 1000 50 {
            [string match {*send-bulk-and-stream*} [migration_status 0 $task_id state]]
        } else {
            fail "ASM task did not start"
        }

        # some write traffic is to have chance to enter streaming buffer state
        set slot0_key "06S"
        R 0 set $slot0_key "a" 

        # after 3 second, the slots snapshot (costs 2s to generate) should be transferred,
        # then start streaming buffer
        after 3000

        set loglines [count_log_lines 0]

        # Start the slot 0 write load on the R 0
        if {$::tls} { set port [lindex [R 0 config get tls-port] 1]
        } else { set port [lindex [R 0 config get port] 1] }
        set load_handle [start_write_load "127.0.0.1" $port 100 $slot0_key]

        # After some time, the client output buffer limit should be reached
        wait_for_log_messages 0 {"*Client * closed * for overcoming of output buffer limits.*"} $loglines 1000 10
        assert_match {*send-bulk-and-stream*} [migration_status 0 $task_id last_error]

        stop_write_load $load_handle

        # resume server and clear pause point
        resume_process $r1_pid
        R 1 debug repl-pause clear

        # Wait for the migration to complete
        wait_for_condition 1000 50 {
            [string match {*done*} [migration_status 0 $task_id state]] &&
            [string match {*done*} [migration_status 1 $task_id state]]
        } else {
            fail "ASM task did not complete successfully"
        }

        # Reset configurations
        R 0 config set client-output-buffer-limit "replica 0 0 0"
        R 0 config set rdb-key-save-delay 0
    }
}
