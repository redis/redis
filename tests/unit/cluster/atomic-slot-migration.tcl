set ::slot_prefixes [dict create \
    0 "{06S}" \
    1 "{Qi}" \
    2 "{5L5}" \
    3 "{4Iu}" \
    4 "{4gY}" \
    5 "{460}" \
    6 "{1Y7}" \
    7 "{1LV}" \
    101 "{1j2}" \
    102 "{75V}" \
    103 "{bno}" \
    5462 "{450}"\
    5463 "{4dY}"\
    6000 "{4L7}" \
    6001 "{4YV}" \
    6002 "{0bx}" \
    6003 "{AJ}" \
    6004 "{of}" \
    16383 "{6ZJ}" \
]

# Helper functions
proc get_port {node_id} {
    if {$::tls} {
        return [lindex [R $node_id config get tls-port] 1]
    } else {
        return [lindex [R $node_id config get port] 1]
    }
}

# return the prefix for the given slot
proc slot_prefix {slot} {
    return [dict get $::slot_prefixes $slot]
}

# return a key for the given slot
proc slot_key {slot {suffix ""}} {
    return "[slot_prefix $slot]$suffix"
}

# Populate a slot with keys
# TODO: Consider merging with populate()
proc populate_slot {num args} {
    # Default values
    set prefix "key:"
    set size 3
    set idx 0
    set prints false
    set expires 0
    set slot -1

    # Parse named arguments
    foreach {key value} $args {
        switch -- $key {
            -prefix { set prefix $value }
            -size { set size $value }
            -idx { set idx $value }
            -prints { set prints $value }
            -expires { set expires $value }
            -slot { set slot $value }
            default { error "Unknown option: $key" }
        }
    }

    # If slot is specified, use slot prefix from table
    if {$slot >= 0} {
        if {[dict exists $::slot_prefixes $slot]} {
            set prefix [dict get $::slot_prefixes $slot]
        } else {
            error "Slot $slot not supported in slot_prefixes table, add it manually"
        }
    }

    R $idx deferred 1
    if {$num > 16} {set pipeline 16} else {set pipeline $num}
    set val [string repeat A $size]
    for {set j 0} {$j < $pipeline} {incr j} {
        if {$expires > 0} {
            R $idx set $prefix$j $val ex $expires
        } else {
            R $idx set $prefix$j $val
        }
        if {$prints} {puts $j}
    }
    for {} {$j < $num} {incr j} {
        if {$expires > 0} {
            R $idx set $prefix$j $val ex $expires
        } else {
            R $idx set $prefix$j $val
        }
        R $idx read
        if {$prints} {puts $j}
    }
    for {set j 0} {$j < $pipeline} {incr j} {
        R $idx read
        if {$prints} {puts $j}
    }
    R $idx deferred 0
}

# Return 1 if all instances are idle
proc asm_all_instances_idle {total} {
    for {set i 0} {$i < $total} {incr i} {
        if {[CI $i cluster_slot_migration_active_tasks] != 0} { return 0 }
        if {[CI $i cluster_slot_migration_active_trim_running] != 0} { return 0 }
    }
    return 1
}

# Wait for all ASM tasks to complete in the cluster
proc wait_for_asm_done {} {
    set total_instances [expr {$::cluster_master_nodes + $::cluster_replica_nodes}]

    wait_for_condition 1000 10 {
        [asm_all_instances_idle $total_instances] == 1
    } else {
        # Print the number of active tasks on each instance
        for {set i 0} {$i < $total_instances} {incr i} {
            set migration_count [CI $i cluster_slot_migration_active_tasks]
            set trim_count [CI $i cluster_slot_migration_active_trim_running]
            puts "Instance $i: migration_tasks=$migration_count, trim_tasks=$trim_count"
        }
        fail "ASM tasks did not complete on all instances"
    }
    # wait all nodes to reach the same cluster config after ASM
    wait_for_cluster_propagation
}

proc failover_and_wait_for_done {node_id {failover_arg ""}} {
    set max_attempts 5
    for {set attempt 1} {$attempt <= $max_attempts} {incr attempt} {
        if {$failover_arg eq ""} {
            R $node_id cluster failover
        } else {
            R $node_id cluster failover $failover_arg
        }

        set completed 1
        wait_for_condition 1000 10 {
            [string match "*master*" [R $node_id role]]
        } else {
            set completed 0
        }

        if {$completed} {
            wait_for_cluster_propagation
            return
        }
    }
    fail "Failover did not complete after $max_attempts attempts for node $node_id"
}

proc migration_status {node_id task_id field} {
    set status [R $node_id CLUSTER MIGRATION STATUS ID $task_id]

    # STATUS ID returns single task, so get first element
    if {[llength $status] == 0} {
        return ""
    }

    set task_status [lindex $status 0]
    set field_value ""

    # Parse the key-value pairs in the task
    for {set i 0} {$i < [llength $task_status]} {incr i 2} {
        set key [lindex $task_status $i]
        set value [lindex $task_status [expr $i + 1]]

        if {$key eq $field} {
            set field_value $value
            break
        }
    }

    return $field_value
}

# Setup slot migration test with keys and delay, then start migration
# Returns the task_id for the migration
proc setup_slot_migration_with_delay {src_node dst_node start_slot end_slot {keys 2} {delay 1000000}} {
    # Two keys on the start slot
    populate_slot $keys -idx $src_node -slot $start_slot

    # we set a delay to ensure migration takes time for testing,
    # with default parameters, two keys cost 2s to save
    R $src_node config set rdb-key-save-delay $delay

    # migrate slot range from src_node to dst_node
    set task_id [R $dst_node CLUSTER MIGRATION IMPORT $start_slot $end_slot]
    wait_for_condition 2000 10 {
        [string match {*send-bulk-and-stream*} [migration_status $src_node $task_id state]]
    } else {
        fail "ASM task did not start"
    }

    return $task_id
}

# Helper function to clear module internal event logs
proc clear_module_event_log {} {
    for {set i 0} {$i < $::cluster_master_nodes + $::cluster_replica_nodes} {incr i} {
        R $i asm.clear_event_log
    }
}

proc reset_default_trim_method {} {
    for {set i 0} {$i < $::cluster_master_nodes + $::cluster_replica_nodes} {incr i} {
        R $i debug asm-trim-method default
    }
}

start_cluster 3 3 {tags {external:skip cluster} overrides {cluster-node-timeout 60000 cluster-allow-replica-migration no}} {
    foreach trim_method {"active" "bg"} {
        test "Simple slot migration (trim method: $trim_method)" {
            R 0 debug asm-trim-method $trim_method
            R 3 debug asm-trim-method $trim_method

            set slot0_key [slot_key 0 mykey]
            R 0 set $slot0_key "a"
            set slot1_key [slot_key 1 mykey]
            R 0 set $slot1_key "b"
            set slot101_key [slot_key 101 mykey]
            R 0 set $slot101_key "c"
            # 3 keys cost 3s to save
            R 0 config set rdb-key-save-delay 1000000

            # load a function
            R 0 function load {#!lua name=test1
                    redis.register_function('test1', function() return 'hello1' end)
            }

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
                R 0 multi
                R 0 append $slot0_key "a"
                R 0 exec
                R 0 append $slot1_key "b"
                R 0 append $slot101_key "c"
            }

            # wait until migration of 0-100 successful
            wait_for_asm_done

            # verify task state became completed
            assert_equal "completed" [migration_status 0 $task_id state]
            assert_equal "completed" [migration_status 1 $task_id state]

            # the appended 99 times should also be migrated
            assert_equal [string repeat a 100] [R 1 get $slot0_key]
            assert_equal [string repeat b 100] [R 1 get $slot1_key]

            # function should be migrated
            assert_equal [R 0 function dump] [R 1 function dump]
            # the slave should also get the data
            wait_for_ofs_sync [Rn 1] [Rn 4]

            R 4 readonly
            assert_equal [string repeat a 100] [R 4 get $slot0_key]
            assert_equal [string repeat b 100] [R 4 get $slot1_key]
            assert_equal [R 0 function dump] [R 4 function dump]

            # verify key that was not in the slot range is not migrated
            assert_equal [string repeat c 100] [R 0 get $slot101_key]
            # verify changes in replica
            wait_for_ofs_sync [Rn 0] [Rn 3]
            R 3 readonly
            assert_equal [string repeat c 100] [R 3 get $slot101_key]

            # cleanup
            R 0 config set rdb-key-save-delay 0
            R 0 flushall
            R 0 function flush
            R 1 flushall
            R 1 function flush
            R 0 CLUSTER MIGRATION IMPORT 0 100
            wait_for_asm_done
        }
    }
}

# Skip most of the tests when running under valgrind since it is hard to
# stabilize tests under valgrind.
if {!$::valgrind} {
start_cluster 3 3 {tags {external:skip cluster} overrides {cluster-node-timeout 60000 cluster-allow-replica-migration no}} {
    test "Test CLUSTER MIGRATION IMPORT input validation" {
        # invalid arguments
        assert_error {*wrong number of arguments*} {R 0 CLUSTER MIGRATION}
        assert_error {*wrong number of arguments*} {R 0 CLUSTER MIGRATION IMPORT}
        assert_error {*wrong number of arguments*} {R 0 CLUSTER MIGRATION IMPORT 100}
        assert_error {*wrong number of arguments*} {R 0 CLUSTER MIGRATION IMPORT 100 200 300}
        assert_error {*unknown argument*} {R 0 CLUSTER MIGRATION UNKNOWN 1 2}

        # invalid slot range
        assert_error {*greater than end slot number*} {R 0 CLUSTER MIGRATION IMPORT 200 100}
        assert_error {*out of range slot*} {R 0 CLUSTER MIGRATION IMPORT 17000 18000}
        assert_error {*out of range slot*} {R 0 CLUSTER MIGRATION IMPORT 14000 18000}
        assert_error {*out of range slot*} {R 0 CLUSTER MIGRATION IMPORT 0 16384}
        assert_error {*out of range slot*} {R 0 CLUSTER MIGRATION IMPORT 0 -1}
        assert_error {*out of range slot*} {R 0 CLUSTER MIGRATION IMPORT -1 2}
        assert_error {*out of range slot*} {R 0 CLUSTER MIGRATION IMPORT -2 -1}
        assert_error {*out of range slot*} {R 0 CLUSTER MIGRATION IMPORT 10 a}
        assert_error {*out of range slot*} {R 0 CLUSTER MIGRATION IMPORT sd sd}
        assert_error {*already the owner of the slot*} {R 0 CLUSTER MIGRATION IMPORT 100 200}
    }

    test "Test CLUSTER MIGRATION CANCEL input validation" {
        # invalid arguments
        assert_error {*wrong number of arguments*} {R 0 CLUSTER MIGRATION CANCEL}
        assert_error {*wrong number of arguments*} {R 0 CLUSTER MIGRATION CANCEL ID}
        assert_error {*wrong number of arguments*} {R 0 CLUSTER MIGRATION CANCEL ID 12345 EXTRAARG}
        assert_error {*wrong number of arguments*} {R 0 CLUSTER MIGRATION CANCEL ALL EXTRAARG}
        assert_error {*unknown argument*} {R 0 CLUSTER MIGRATION CANCEL UNKNOWNARG}
        assert_error {*unknown argument*} {R 0 CLUSTER MIGRATION CANCEL abc def}
        # empty string id should not cancel any task
        assert_equal 0 [R 0 CLUSTER MIGRATION CANCEL ID ""]
    }

    test "Test CLUSTER MIGRATION STATUS input validation" {
        # invalid arguments
        assert_error {*wrong number of arguments*} {R 0 CLUSTER MIGRATION STATUS}
        assert_error {*wrong number of arguments*} {R 0 CLUSTER MIGRATION STATUS ID}
        assert_error {*wrong number of arguments*} {R 0 CLUSTER MIGRATION STATUS ID id EXTRAARG}
        assert_error {*wrong number of arguments*} {R 0 CLUSTER MIGRATION STATUS ALL EXTRAARG}
        assert_error {*unknown argument*} {R 0 CLUSTER MIGRATION STATUS ABC DEF}
        assert_error {*unknown argument*} {R 0 CLUSTER MIGRATION STATUS UNKNOWNARG}
        # empty string id should not list any task
        assert_equal {} [R 0 CLUSTER MIGRATION STATUS ID ""]
    }

    test "Test TRIMSLOTS input validation" {
        # Wrong number of arguments
        assert_error {*wrong number of arguments*} {R 0 TRIMSLOTS}
        assert_error {*wrong number of arguments*} {R 0 TRIMSLOTS RANGES}
        assert_error {*wrong number of arguments*} {R 0 TRIMSLOTS RANGES 1}
        assert_error {*wrong number of arguments*} {R 0 TRIMSLOTS RANGES 2 100}
        assert_error {*wrong number of arguments*} {R 0 TRIMSLOTS RANGES 17000 1}
        assert_error {*wrong number of arguments*} {R 0 TRIMSLOTS RANGES abc}

        # Missing ranges argument
        assert_error {*missing ranges argument*} {R 0 TRIMSLOTS UNKNOWN 1 100 200}

        # Invalid number of ranges
        assert_error {*invalid number of ranges*} {R 0 TRIMSLOTS RANGES 0 1 1}
        assert_error {*invalid number of ranges*} {R 0 TRIMSLOTS RANGES -1 2 2}
        assert_error {*invalid number of ranges*} {R 0 TRIMSLOTS RANGES 17000 1 2}
        assert_error {*invalid number of ranges*} {R 0 TRIMSLOTS RANGES 2 100 200 300}

        # Invalid slot numbers
        assert_error {*out of range slot*} {R 0 TRIMSLOTS RANGES 1 -1 0}
        assert_error {*out of range slot*} {R 0 TRIMSLOTS RANGES 1 -2 -1}
        assert_error {*out of range slot*} {R 0 TRIMSLOTS RANGES 1 0 16384}
        assert_error {*out of range slot*} {R 0 TRIMSLOTS RANGES 1 abc def}
        assert_error {*out of range slot*} {R 0 TRIMSLOTS RANGES 1 100 abc}

        # Start slot greater than end slot
        assert_error {*greater than end slot number*} {R 0 TRIMSLOTS RANGES 1 200 100}
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

    test "Test CLUSTER MIGRATION STATUS ALL lists all tasks" {
        # Create 3 completed tasks
        R 0 CLUSTER MIGRATION IMPORT 7000 7001
        wait_for_asm_done
        R 0 CLUSTER MIGRATION IMPORT 7002 7003
        wait_for_asm_done
        R 0 CLUSTER MIGRATION IMPORT 7004 7005
        wait_for_asm_done

        # Get node IDs for verification
        set node0_id [R 0 cluster myid]
        set node1_id [R 1 cluster myid]

        # Verify CLUSTER MIGRATION STATUS ALL reply from both nodes
        foreach node_idx {0 1} {
            set tasks [R $node_idx CLUSTER MIGRATION STATUS ALL]
            assert_equal 3 [llength $tasks]

            for {set i 0} {$i < 3} {incr i} {
                set task [lindex $tasks $i]

                # Verify field order
                set expected_fields {id slots source dest operation state
                                    last_error retries create_time start_time
                                    end_time write_pause_ms}
                for {set j 0} {$j < [llength $expected_fields]} {incr j} {
                    set expected_field [lindex $expected_fields $j]
                    set actual_field [lindex $task [expr $j * 2]]
                    assert_equal $expected_field $actual_field
                }

                # Verify basic fields
                assert_equal "completed" [dict get $task state]
                assert_equal "" [dict get $task last_error]
                assert_equal 0 [dict get $task retries]
                assert {[dict get $task write_pause_ms] >= 0}

                # Verify operation based on node
                if {$node_idx == 0} {
                    assert_equal "import" [dict get $task operation]
                } else {
                    assert_equal "migrate" [dict get $task operation]
                }

                # Verify node IDs (all tasks: node1 -> node0)
                assert_equal $node1_id [dict get $task source]
                assert_equal $node0_id [dict get $task dest]

                # Verify timestamps exist and are reasonable
                set create_time [dict get $task create_time]
                set start_time [dict get $task start_time]
                set end_time [dict get $task end_time]
                assert {$create_time > 0}
                assert {$start_time >= $create_time}
                assert {$end_time >= $start_time}

                # Verify specific slot ranges for each task
                set slots [dict get $task slots]
                if {$i == 0} {
                    assert_equal "7004-7005" $slots
                } elseif {$i == 1} {
                    assert_equal "7002-7003" $slots
                } elseif {$i == 2} {
                    assert_equal "7000-7001" $slots
                }
            }
        }

        # cleanup
        R 1 CLUSTER MIGRATION IMPORT 7000 7005
        wait_for_asm_done
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
            [string match {*completed*} [migration_status 0 $task_id state]] &&
            [string match {*completed*} [migration_status 1 $task_id state]]
        } else {
            fail "ASM task did not start"
        }
        assert_equal "tag22273" [R 0 get tag22273]
        assert_equal "tag9283" [R 0 get tag9283]
        R 1 config set rdb-key-save-delay 0

        # revert the migration
        R 1 CLUSTER MIGRATION IMPORT 7000 8000
        wait_for_asm_done
    }

    test "Test IMPORT with unsorted and adjacent ranges" {
        # Redis should sort and merge adjacent ranges
        # Adjacent means: prev.end + 1 == next.start
        # e.g. 7000-7001 7002-7003 7004-7005  =>  7000-7005

        # Test with adjacent ranges
        set task_id [R 0 CLUSTER MIGRATION IMPORT 7000 7001 7002 7100]
        wait_for_asm_done
        # verify migration is successfully completed on both nodes
        assert_equal "completed" [migration_status 0 $task_id state]
        assert_equal "completed" [migration_status 1 $task_id state]
        # verify slot ranges are merged correctly
        assert_equal "7000-7100" [migration_status 0 $task_id slots]
        assert_equal "7000-7100" [migration_status 1 $task_id slots]

        # Test with unsorted and adjacent ranges
        set task_id [R 1 CLUSTER MIGRATION IMPORT 7050 7051 7010 7049 7000 7005]
        wait_for_asm_done
        # verify migration is successfully completed on both nodes
        assert_equal "completed" [migration_status 0 $task_id state]
        assert_equal "completed" [migration_status 1 $task_id state]
        # verify slot ranges are merged correctly
        assert_equal "7000-7005 7010-7051" [migration_status 0 $task_id slots]
        assert_equal "7000-7005 7010-7051" [migration_status 1 $task_id slots]

        # Another test with unsorted and adjacent ranges
        set task_id [R 1 CLUSTER MIGRATION IMPORT 7007 7007 7008 7009 7006 7006]
        wait_for_asm_done
        # verify migration is successfully completed on both nodes
        assert_equal "completed" [migration_status 0 $task_id state]
        assert_equal "completed" [migration_status 1 $task_id state]
        # verify slot ranges are merged correctly
        assert_equal "7006-7009" [migration_status 0 $task_id slots]
        assert_equal "7006-7009" [migration_status 1 $task_id slots]
    }

    test "Simple slot migration with write load" {
        # Perform slot migration while traffic is on and verify data consistency.
        # Trimming is disabled on source nodes so, we can compare the dbs after
        # migration via DEBUG DIGEST to ensure no data loss during migration.
        # Steps:
        # 1. Disable trimming on both nodes
        # 2. Populate slot 0 on node-0 and slot 6000 on node-1
        # 2. Start write traffic on both nodes
        # 3. Migrate slot 0 from node-0 to node-1
        # 4. Migrate slot 6000 from node-1 to node-0
        # 5. Stop write traffic, verify db's are identical.

        # This test runs slowly under the thread sanitizer.
        #  1. Increase the lag threshold from the default 1 MB to 10 MB to let the destination catch up easily.
        #  2. Increase the write pause timeout from the default 10s to 60s so the source can wait longer.
        set prev_config_lag [lindex [R 0 config get cluster-slot-migration-handoff-max-lag-bytes] 1]
        R 0 config set cluster-slot-migration-handoff-max-lag-bytes 10mb
        R 1 config set cluster-slot-migration-handoff-max-lag-bytes 10mb
        set prev_config_timeout [lindex [R 0 config get cluster-slot-migration-write-pause-timeout] 1]
        R 0 config set cluster-slot-migration-write-pause-timeout 60000
        R 1 config set cluster-slot-migration-write-pause-timeout 60000

        R 0 flushall
        R 0 debug asm-trim-method none
        populate_slot 10000 -idx 0 -slot 0

        R 1 flushall
        R 1 debug asm-trim-method none
        populate_slot 10000 -idx 1 -slot 6000

        # Start write traffic on node-0
        # Throws -MOVED error once asm is completed, catch block will ignore it.
        catch {
            # Start the slot 0 write load on the R 0
            set port [get_port 0]
            set key [slot_key 0 mykey]
            set load_handle0 [start_write_load "127.0.0.1" $port 100 $key 0 5]
        }

        # Start write traffic on node-1
        # Throws -MOVED error once asm is completed, catch block will ignore it.
        catch {
            # Start the slot 6000 write load on the R 1
            set port [get_port 1]
            set key [slot_key 6000 mykey]
            set load_handle1 [start_write_load "127.0.0.1" $port 100 $key 0 5]
        }

        # Migrate keys
        R 1 CLUSTER MIGRATION IMPORT 0 100
        wait_for_asm_done
        R 0 CLUSTER MIGRATION IMPORT 6000 6100
        wait_for_asm_done

        stop_write_load $load_handle0
        stop_write_load $load_handle1

        # verify data
        assert_morethan [R 0 dbsize] 0
        assert_equal [R 0 debug digest] [R 1 debug digest]

        # cleanup
        R 0 config set cluster-slot-migration-handoff-max-lag-bytes $prev_config_lag
        R 0 config set cluster-slot-migration-write-pause-timeout $prev_config_timeout
        R 0 debug asm-trim-method default
        R 0 flushall
        R 1 config set cluster-slot-migration-handoff-max-lag-bytes $prev_config_lag
        R 1 config set cluster-slot-migration-write-pause-timeout $prev_config_timeout
        R 1 debug asm-trim-method default
        R 1 flushall

        R 1 CLUSTER MIGRATION IMPORT 6000 6100
        wait_for_asm_done
    }

    test "Verify expire time is migrated correctly" {
        R 0 flushall
        R 1 flushall

        set string_key [slot_key 0 string_key]
        set list_key [slot_key 0 list_key]
        set hash_key [slot_key 0 hash_key]
        set stream_key [slot_key 0 stream_key]

        for {set i 0} {$i < 20} {incr i} {
            R 1 hset $hash_key $i $i
            R 1 xadd $stream_key * item $i
        }
        for {set i 0} {$i < 2000} {incr i} {
            R 1 lpush $list_key $i
        }

        # set expire time of some keys
        R 1 set $string_key "a" EX 1000
        R 1 EXPIRE $list_key 1000
        R 1 EXPIRE $hash_key 1000

        # migrate slot 0-100 to R 0
        R 0 CLUSTER MIGRATION IMPORT 0 100
        wait_for_asm_done

        # check expire times are migrated correctly
        assert_range [R 0 ttl $string_key] 900 1000
        assert_range [R 0 ttl $list_key] 900 1000
        assert_range [R 0 ttl $hash_key] 900 1000
        assert_equal -1 [R 0 ttl $stream_key]

        # cleanup
        R 0 flushall
        R 1 flushall
        R 1 CLUSTER MIGRATION IMPORT 0 100
        wait_for_asm_done
    }

    test "Slot migration with complex data types can work well" {
        R 0 flushall
        R 1 flushall

        set list_key [slot_key 0 list_key]
        set set_key [slot_key 0 set_key]
        set zset_key [slot_key 0 zset_key]
        set hash_key [slot_key 0 hash_key]
        set stream_key [slot_key 0 stream_key]

        # generate big keys for each data type
        for {set i 0} {$i < 1000} {incr i} {
            R 1 lpush $list_key $i
            R 1 sadd $set_key $i
            R 1 zadd $zset_key $i $i
            R 1 hset $hash_key $i $i
            R 1 xadd $stream_key * item $i
        }

        # migrate slot 0-100 to R 0
        R 0 CLUSTER MIGRATION IMPORT 0 100
        wait_for_asm_done
        # check the data on destination node is correct
        assert_equal 1000 [R 0 llen $list_key]
        assert_equal 1000 [R 0 scard $set_key]
        assert_equal 1000 [R 0 zcard $zset_key]
        assert_equal 1000 [R 0 hlen $hash_key]
        assert_equal 1000 [R 0 xlen $stream_key]
        # migrate slot 0-100 to R 1
        R 1 CLUSTER MIGRATION IMPORT 0 100
        wait_for_asm_done
    }

    proc asm_basic_error_handling_test {operation channel all_states} {
        foreach state $all_states {
            if {$::verbose} { puts "Testing $operation $channel channel with state: $state"}

            # For states that need incremental data streaming, set a longer delay
            set streaming_states [list "streaming-buffer" "accumulate-buffer" "send-bulk-and-stream" "send-stream"]
            if {$state in $streaming_states} {
                R 1 config set rdb-key-save-delay 1000000
            }

            # Let the destination node take time to stream buffer, so the source node will handle
            # slot snapshot child process exit, and then enter "send-stream" state.
            if {$state == "send-stream"} {
                R 0 config set key-load-delay 100000
            }

            # Start the slot 0 write load on the R 1
            set slot0_key [slot_key 0 mykey]
            set load_handle [start_write_load "127.0.0.1" [get_port 1] 100 $slot0_key 500]

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
            wait_for_condition 2000 10 {
                [string match -nocase "*$channel*${state}*" [migration_status 0 $task_id last_error]] ||
                [string match -nocase "*$channel*${state}*" [migration_status 1 $task_id last_error]]
            } else {
                fail "ASM task did not fail with expected error -
                     (dst: [migration_status 0 $task_id last_error]
                      src: [migration_status 1 $task_id last_error]
                      expected: $channel $state)"
            }
            stop_write_load $load_handle

            # Cancel the task
            R 0 CLUSTER MIGRATION CANCEL ID $task_id
            R 1 CLUSTER MIGRATION CANCEL ID $task_id

            R 1 config set rdb-key-save-delay 0
            R 0 config set key-load-delay 0
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
            "send-stream" \
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
        R 0 flushall
        R 1 flushall
        set slot0_key [slot_key 0 mykey]
        set slot1_key [slot_key 1 mykey]
        R 1 set $slot0_key "a"
        R 1 set $slot1_key "b"

        # we set a delay to write incremental data
        R 1 config set rdb-key-save-delay 1000000

        # Start the slot 0 write load on the R 1
        set load_handle [start_write_load "127.0.0.1" [get_port 1] 100 $slot0_key]

        # Clear all fail points
        assert_equal {OK} [R 0 debug asm-failpoint "" ""]
        assert_equal {OK} [R 1 debug asm-failpoint "" ""]

        # Start the migration
        set task_id [R 0 CLUSTER MIGRATION IMPORT 0 100]

        # Wait for the migration to complete
        wait_for_asm_done

        stop_write_load $load_handle

        # Verify the data is migrated, slot 0 and 1 should belong to R 1
        # slot 0 key should be changed by the write load
        assert_not_equal "a" [R 0 get $slot0_key]
        assert_equal "b" [R 0 get $slot1_key]
        R 1 config set rdb-key-save-delay 0
    }

    test "Client output buffer limit is reached on source side" {
        R 0 flushall
        R 1 flushall
        set r1_pid [S 1 process_id]
        R 1 debug repl-pause on-streaming-repl-buf

        # Set a small output buffer limit to trigger the error
        R 0 config set client-output-buffer-limit "replica 4mb 0 0"

        set task_id [setup_slot_migration_with_delay 0 1 0 100]

        # some write traffic is to have chance to enter streaming buffer state
        set slot0_key [slot_key 0 mykey]
        R 0 set $slot0_key "a"

        # after 3 second, the slots snapshot (costs 2s to generate) should be transferred,
        # then start streaming buffer
        after 3000

        set loglines [count_log_lines 0]

        # Start the slot 0 write load on the R 0
        set load_handle [start_write_load "127.0.0.1" [get_port 0] 100 $slot0_key 1000]

        # verify the metric is accessible, it is transient, will be reset on disconnect
        assert {[S 0 mem_cluster_slot_migration_output_buffer] >= 0}

        # After some time, the client output buffer limit should be reached
        wait_for_log_messages 0 {"*Client * closed * for overcoming of output buffer limits.*"} $loglines 1000 10
        wait_for_condition 1000 10 {
            [string match {*send*stream*} [migration_status 0 $task_id last_error]]
        } else {
            fail "ASM task did not fail as expected"
        }

        stop_write_load $load_handle

        # Reset configurations
        R 0 config set client-output-buffer-limit "replica 0 0 0"
        R 0 config set rdb-key-save-delay 0

        # resume server and clear pause point
        resume_process $r1_pid
        R 1 debug repl-pause clear

        # Wait for the migration to complete
        wait_for_asm_done
    }

    test "Full sync buffer limit is reached on destination side" {
        # Set a small replication buffer limit to trigger the error
        R 0 config set replica-full-sync-buffer-limit 1mb

        # start migration from 1 to 0, cost 4s to transfer slots snapshot
        set task_id [setup_slot_migration_with_delay 1 0 0 100 2 2000000]
        set loglines [count_log_lines 0]

        # Create some traffic on slot 0
        populate_slot 100 -idx 1 -slot 0 -size 100000

        # After some time, slots sync buffer limit should be reached, but migration would not fail
        # since the buffer will be accumulated on source side from now.
        wait_for_log_messages 0 {"*Slots sync buffer limit has been reached*"} $loglines 1000 10

        # verify the peak value, should be greater than 1mb
        assert {[S 0 mem_cluster_slot_migration_input_buffer_peak] > 1000000}
        # verify the metric is accessible, it is transient, will be reset on disconnect
        assert {[S 0 mem_cluster_slot_migration_input_buffer] >= 0}

        wait_for_asm_done

        # Reset configurations
        R 0 config set replica-full-sync-buffer-limit 0
        R 1 config set rdb-key-save-delay 0
        R 1 cluster migration import 0 100
        wait_for_asm_done
    }

    test "Expired key is not deleted and SCAN/KEYS/RANDOMKEY/CLUSTER GETKEYSINSLOT filter keys in importing slots" {
        set slot0_key [slot_key 0 mykey]
        set slot1_key [slot_key 1 mykey]
        set slot2_key [slot_key 2 mykey]
        R 1 flushall
        R 0 flushall

        # we set a delay to write incremental data
        R 1 config set rdb-key-save-delay 1000000

        # set expire time 2s. Generating slot snapshot will 3s, so these
        # three keys will be expired after slot snapshot is transferred
        R 1 setex $slot0_key 2 "a"
        R 1 setex $slot1_key 2 "b"
        R 1 hset $slot2_key "f1" "1"
        R 1 expire $slot2_key 2
        R 1 hexpire $slot2_key 2 FIELDS 1 "f1"

        set task_id [R 0 CLUSTER MIGRATION IMPORT 0 100]
        wait_for_condition 2000 10 {
            [string match {*send-bulk-and-stream*} [migration_status 1 $task_id state]]
        } else {
            fail "ASM task did not start"
        }

        # update expire time during mirgration
        R 1 setex $slot0_key 100 "a"
        R 1 expire $slot1_key 80
        R 1 expire $slot2_key 60
        R 1 hincrbyfloat $slot2_key "f1" 1
        R 1 hexpire $slot2_key 60 FIELDS 1 "f1"

        # after 2s, at least a key should be transferred, and should not be deleted
        # due to expired, neither active nor lazy expiration (SCAN) takes effect,
        # Besides SCAN/KEYS/RANDOMKEY/CLUSTER GETKEYSINSLOT command can not find them
        after 2000
        R 3 readonly
        foreach id {0 3} { ;# 0 is the master, 3 is the replica
            assert_equal {0 {}} [R $id scan 0 count 10]
            assert_equal {} [R $id keys "*"]
            assert_equal {} [R $id keys "{06S}*"]
            assert_equal {} [R $id randomkey]
            assert_equal {} [R $id cluster getkeysinslot 0 100]
            assert_equal [R $id cluster countkeysinslot 0] 0
            assert_equal [R $id dbsize] 0

            # but we can see the number of keys is increased in INFO KEYSPACE
            assert {[scan [regexp -inline {keys\=([\d]*)} [R $id info keyspace]] keys=%d] >= 1}
            assert {[scan [regexp -inline {expires\=([\d]*)} [R $id info keyspace]] expires=%d] >= 1}
        }

        wait_for_asm_done

        wait_for_ofs_sync [Rn 0] [Rn 3]

        foreach id {0 3} { ;# 0 is the master, 3 is the replica
            # verify the keys are valid
            assert_range [R $id ttl $slot0_key] 90 100
            assert_range [R $id ttl $slot1_key] 70 80
            assert_range [R $id ttl $slot2_key] 50 60
            assert_range [R $id httl $slot2_key FIELDS 1 "f1"] 50 60

            # KEYS/SCAN/RANDOMKEY/CLUSTER GETKEYSINSLOT will find the keys after migration
            assert_equal [list 0 [list $slot0_key $slot1_key $slot2_key]] [R $id scan 0 count 10]
            assert_equal [list $slot0_key $slot1_key $slot2_key] [R $id keys "*"]
            assert_equal [list $slot0_key] [R $id keys "{06S}*"]
            assert_not_equal {} [R $id randomkey]
            assert_equal [list $slot0_key] [R $id cluster getkeysinslot 0 100]

            # INFO KEYSPACE/DBSIZE/CLUSTER COUNTKEYSINSLOT will also reflect the keys
            assert_equal 3 [scan [regexp -inline {keys\=([\d]*)} [R $id info keyspace]] keys=%d]
            assert_equal 3 [scan [regexp -inline {expires\=([\d]*)} [R $id info keyspace]] expires=%d]
            assert_equal 1 [scan [regexp -inline {subexpiry\=([\d]*)} [R $id info keyspace]] subexpiry=%d]
            assert_equal 3 [R $id dbsize]
            assert_equal 1 [R $id cluster countkeysinslot 0]
        }

        # update expire time to 10ms, after some time, the keys should be deleted due to
        # active expiration
        R 0 pexpire $slot0_key 10
        R 0 pexpire $slot1_key 10
        R 0 hpexpire $slot2_key 10 FIELDS 1 "f1" ;# the last field is expired, the key will be deleted
        wait_for_condition 100 50 {
            [scan [regexp -inline {keys\=([\d]*)} [R 0 info keyspace]] keys=%d] == {} &&
            [scan [regexp -inline {keys\=([\d]*)} [R 3 info keyspace]] keys=%d] == {}
        } else {
            fail "keys did not expire"
        }

        R 1 config set rdb-key-save-delay 0
    }

    test "Eviction does not evict keys in importing slots" {
        set slot0_key [slot_key 0 mykey]
        set slot1_key [slot_key 1 mykey]
        set slot2_key [slot_key 2 mykey]
        set slot5462_key [slot_key 5462 mykey]
        set slot5463_key [slot_key 5463 mykey]
        R 1 flushall
        R 0 flushall

        # we set a delay to write incremental data
        R 0 config set rdb-key-save-delay 1000000

        set 1k_str [string repeat "a" 1024]
        set 1m_str [string repeat "a" 1048576]

        # set two keys to be evicted
        R 1 set $slot5462_key $1k_str
        R 1 set $slot5463_key $1k_str

        # set maxmemory to 200kb more than current used memory,
        # redis should evict some keys if importing some big keys
        set r1_mem_used [S 1 used_memory]
        set r1_max_mem [expr {$r1_mem_used + 200*1024}]
        R 1 config set maxmemory $r1_max_mem
        R 1 config set maxmemory-policy allkeys-lru

        # set 3 keys to be migrated
        R 0 set $slot0_key $1m_str
        R 0 set $slot1_key $1m_str
        R 0 set $slot2_key $1m_str

        set task_id [R 1 CLUSTER MIGRATION IMPORT 0 100]
        wait_for_condition 2000 10 {
            [string match {*send-bulk-and-stream*} [migration_status 0 $task_id state]]
        } else {
            fail "ASM task did not start"
        }

        # after 2.2s, at least two keys should be transferred, they should not be evicted
        # but other keys (slot5462_key and slot5463_key) should be evicted
        after 2200
        for {set j 0} {$j < 100} {incr j} { R 1 ping } ;# trigger eviction
        assert_equal 0 [R 1 exists $slot5462_key]
        assert_equal 0 [R 1 exists $slot5463_key]
        assert {[scan [regexp -inline {keys\=([\d]*)} [R 1 info keyspace]] keys=%d] >= 2}

        # current used memory should be more than the maxmemory, since the big keys that
        # belong importing slots can not be evicted.
        set r1_mem_used  [S 1 used_memory]
        assert {$r1_mem_used > $r1_max_mem + 1024*1024}

        wait_for_asm_done

        # after migration, these big keys should be evicted
        for {set j 0} {$j < 100} {incr j} { R 1 ping } ;# trigger eviction
        assert_equal {} [scan [regexp -inline {expires\=([\d]*)} [R 1 info keyspace]] expires=%d]
    }

    test "Failover will cancel slot migration tasks" {
        # migrate slot 0-100 from 1 to 0
        set task_id [setup_slot_migration_with_delay 1 0 0 100]

        # FAILOVER happens on the destination node, instance #3 become master, #0 become slave
        failover_and_wait_for_done 3

        # the old master will cancel the importing task, and the migrating task on
        # the source node will be failed
        wait_for_condition 1000 50 {
            [string match {*canceled*} [migration_status 0 $task_id state]] &&
            [string match {*failover*} [migration_status 0 $task_id last_error]] &&
            [string match {*failed*} [migration_status 1 $task_id state]]
        } else {
            fail "ASM task did not cancel"
        }

        # We can restart ASM tasks on new master, migrate slot 0-100 from 1 to 3
        R 1 config set rdb-key-save-delay 0
        set task_id [R 3 CLUSTER MIGRATION IMPORT 0 100]
        wait_for_asm_done

        # migrate slot 0-100 from 3 to 1
        set task_id [setup_slot_migration_with_delay 3 1 0 100]

        # FAILOVER happens on the source node, instance #3 become slave, #0 become master
        failover_and_wait_for_done 0

        # the old master will cancel the migrating task, but the destination node will
        # retry the importing task, and then succeed.
        wait_for_condition 1000 50 {
            [string match {*canceled*} [migration_status 3 $task_id state]]
        } else {
            fail "ASM task did not cancel"
        }
        wait_for_asm_done
    }

    test "Flush-like command can cancel slot migration task" {
        # flushall, flushdb
        foreach flushcmd {flushall flushdb} {
            # start slot migration from 1 to 0
            set task_id [setup_slot_migration_with_delay 1 0 0 100]

            if {$::verbose} { puts "Testing flush command: $flushcmd"}
            R 0 $flushcmd

            # flush-like will cancel the task
            wait_for_condition 1000 50 {
                [string match {*canceled*} [migration_status 0 $task_id state]]
            } else {
                fail "ASM task did not cancel"
            }
        }

        R 1 config set rdb-key-save-delay 0
        R 0 cluster migration import 0 100
        wait_for_asm_done
    }

    test "CLUSTER SETSLOT command when there is a slot migration task" {
        # Setup slot migration test from node 0 to node 1
        set task_id [setup_slot_migration_with_delay 0 1 0 100]

        # Cluster SETSLOT command is not allowed when there is a slot migration task
        # on the slot. #0 and #1 are having migration task now.
        foreach instance {0 1} {     
            set node_id [R $instance cluster myid]

            catch {R $instance cluster setslot 0 migrating $node_id} err
            assert_match {*in an active atomic slot migration*} $err

            catch {R $instance cluster setslot 0 importing $node_id} err
            assert_match {*in an active atomic slot migration*} $err

            catch {R $instance cluster setslot 0 stable} err
            assert_match {*in an active atomic slot migration*} $err

            catch {R $instance cluster setslot 0 node $node_id} err
            assert_match {*in an active atomic slot migration*} $err
        }

        # CLUSTER SETSLOT on other node will cancel the migration task, we update
        # the owner of slot 0 (that is migrating from #0 to #1) to #2 on #2, we
        # bump the config epoch to make sure the change can update #0 and #1
        # slot configuration, so #0 and #1 will cancel the migration task.
        # BTW, if config epoch is not bumped, the slot config of #2 may be
        # updated by #0 and #1.
        R 2 cluster bumpepoch
        R 2 cluster setslot 0 node [R 2 cluster myid]
        wait_for_condition 1000 50 {
            [string match {*canceled*} [migration_status 0 $task_id state]] &&
            [string match {*slots configuration updated*} [migration_status 0 $task_id last_error]] &&
            [string match {*canceled*} [migration_status 1 $task_id state]]
        } else {
            fail "ASM task did not cancel"
        }

        # set slot 0 back to #0
        R 0 cluster bumpepoch
        R 0 cluster setslot 0 node [R 0 cluster myid]
        wait_for_cluster_propagation
        wait_for_cluster_state "ok"
    }

    test "CLUSTER DELSLOTSRANGE command cancels a slot migration task" {
        # start slot migration from 0 to 1
        set task_id [setup_slot_migration_with_delay 0 1 0 100]

        R 0 cluster delslotsrange 0 100
        wait_for_condition 1000 50 {
            [string match {*canceled*} [migration_status 0 $task_id state]] &&
            [string match {*slots configuration updated*} [migration_status 0 $task_id last_error]] &&
            [string match {*failed*} [migration_status 1 $task_id state]]
        } else {
            fail "ASM task did not cancel"
        }
        R 1 cluster migration cancel id $task_id

        # add the slots back
        R 0 cluster addslotsrange 0 100
        wait_for_cluster_propagation
        wait_for_cluster_state "ok"
    }

    # NOTE: this test needs more than 60s, maybe you can skip when testing
    test "CLUSTER FORGET command cancels a slot migration task" {
        R 0 config set rdb-key-save-delay 0
        # Migrate all slot on #0 to #1, so we can forget #0
        set task_id [R 1 CLUSTER MIGRATION IMPORT 0 5461]
        wait_for_asm_done

        # start slot migration from 1 to 0
        set task_id [setup_slot_migration_with_delay 1 0 0 5461]

        # Forget #0 on #1, the migration task on #1 will be canceled due to node deleted,
        # and the importing task on #0 will be failed
        R 1 cluster forget [R 0 cluster myid]
        wait_for_condition 1000 50 {
            [string match {*canceled*} [migration_status 1 $task_id state]] &&
            [string match {*node deleted*} [migration_status 1 $task_id last_error]] &&
            [string match {*failed*} [migration_status 0 $task_id state]]
        } else {
            fail "ASM task did not cancel"
        }

        # Add #0 back into cluster
        # NOTE: this will cost 60s to let #0 join the cluster since
        # other nodes add #0 into black list for 60s after FORGET.
        R 1 config set rdb-key-save-delay 0
        R 1 cluster meet "127.0.0.1" [lindex [R 0 config get port] 1]

        # the importing task on #0 will be retried, and eventually succeed
        # since now #0 is back in the cluster
        wait_for_condition 3000 50 {
            [string match {*completed*} [migration_status 0 $task_id state]] &&
            [string match {*completed*} [migration_status 1 $task_id state]]
        } else {
            fail "ASM task did not finish"
        }

        # make sure #0 is completely back to the cluster
        wait_for_cluster_propagation
        wait_for_cluster_state "ok"
    }

    test "CLIENT PAUSE can cancel slot migration task" {
        # start slot migration from 0 to 1
        set task_id [setup_slot_migration_with_delay 0 1 0 100]

        # CLIENT PAUSE happens on the destination node, #1 will cancel the importing task
        R 1 client pause 100000 write ;# pause 100s
        wait_for_condition 1000 50 {
            [string match {*canceled*} [migration_status 1 $task_id state]] &&
            [string match {*client pause*} [migration_status 1 $task_id last_error]]
        } else {
            fail "ASM task did not cancel"
        }

        # start task again
        set task_id [R 1 CLUSTER MIGRATION IMPORT 0 100]
        after 200 ;# give some time to have chance to schedule the task
        # the task should not start since server is paused
        assert {[string match {*none*} [migration_status 1 $task_id state]]}

        # unpause the server, the task should start
        R 1 client unpause
        wait_for_asm_done

        # migrate back to original node #0
        R 0 config set rdb-key-save-delay 0
        R 1 config set rdb-key-save-delay 0
        R 0 CLUSTER MIGRATION IMPORT 0 100
        wait_for_asm_done
    }

    test "Server shutdown can cancel slot migration task, exit with success" {
        # start slot migration from 0 to 1
        setup_slot_migration_with_delay 0 1 0 100

        set loglines [count_log_lines -1]

        # Shutdown the server, it should cancel the migration task
        restart_server -1 true false true nosave

        wait_for_log_messages -1 {"*Cancelled due to server shutdown*"} $loglines 100 100

        wait_for_cluster_propagation
        wait_for_cluster_state "ok"
    }

    test "Cancel import task when streaming buffer into db" {
        # set a delay to have time to cancel import task that is streaming buf to db
        R 1 config set key-load-delay 50000
        # start slot migration from 0 to 1
        set task_id [setup_slot_migration_with_delay 0 1 0 100 5]

        # start the slot 0 write load on the node 0
        set slot0_key [slot_key 0 mykey]
        set load_handle [start_write_load "127.0.0.1" [get_port 0] 100 $slot0_key 500]

        # wait for entering streaming buffer state
        wait_for_condition 1000 10 {
            [string match {*streaming-buffer*} [migration_status 1 $task_id state]]
        } else {
            fail "ASM task did not enter streaming buffer state"
        }
        stop_write_load $load_handle

        # cancel the import task on #1, the destination node works fine
        R 1 cluster migration cancel id $task_id
        assert_match {*canceled*} [migration_status 1 $task_id state]

        # reset config
        R 0 config set key-load-delay 0
        R 1 config set key-load-delay 0
    }

    test "Destination node main channel timeout when waiting stream EOF" {
        set task_id [setup_slot_migration_with_delay 0 1 0 100]
        R 1 config set repl-timeout 5

        # pause the source node to make EOF wait timeout. Do not pause
        # the child process, so it can deliver slot snapshot to destination
        set r0_process_id [S 0 process_id]
        pause_process $r0_process_id

        # the destination node will fail after 7s, 5s for EOF wait and 2s for slot snapshot
        wait_for_condition 1000 20 {
            [string match {*failed*} [migration_status 1 $task_id state]] &&
            [string match {*Main channel*Connection timeout*wait-stream-eof*} \
                [migration_status 1 $task_id last_error]]
        } else {
            fail "ASM task did not fail"
        }

        # resume the source node
        resume_process $r0_process_id

        # After the source node is resumed, the task on source node may receive
        # ACKs from destination and consider the task is stream-done. In this case,
        # the task on source node will be failed after several seconds
        if {[string match {*stream-done*} [migration_status 0 $task_id state]]} {
            wait_for_condition 1000 20 {
                [string match {*failed*} [migration_status 0 $task_id state]] &&
                [string match {*Server paused*} [migration_status 0 $task_id last_error]]
            } else {
                fail "ASM task did not fail"
            }
        }

        R 1 config set repl-timeout 60
        R 0 cluster migration cancel id $task_id
        R 1 cluster migration cancel id $task_id
    }

    test "Destination node rdb channel timeout when transferring slots snapshot" {
        # cost 10s to transfer each key
        set task_id [setup_slot_migration_with_delay 0 1 0 100 2 10000000]
        R 1 config set repl-timeout 3

        # the destination node will fail after 3s
        wait_for_condition 1000 20 {
            [string match {*failed*} [migration_status 1 $task_id state]] &&
            [string match {*RDB channel*Connection timeout*rdbchannel-transfer*} \
                [migration_status 1 $task_id last_error]]
        } else {
            fail "ASM task did not fail"
        }

        R 1 config set repl-timeout 60
        R 0 cluster migration cancel id $task_id
        R 1 cluster migration cancel id $task_id
    }

    test "Source node rdb channel timeout when transferring slots snapshot" {
        set r1_pid [S 1 process_id]
        R 0 flushall
        R 0 config set save ""
        # generate several large keys, make sure the memory usage is more than
        # socket buffer size, so the rdb channel will block and timeout if
        # no data is received by destination.
        set val [string repeat "a" 102400] ;# 100kb
        for {set i 0} {$i < 1000} {incr i} {
            set key [slot_key 0 "key$i"]
            R 0 set $key $val
        }
        R 0 config set repl-timeout 3 ;# 3s for rdb channel timeout
        R 0 config set rdb-key-save-delay 10000 ;# 1000 keys cost 10s to save

        # start migration from #0 to #1
        set task_id [R 1 CLUSTER MIGRATION IMPORT 0 100]
        wait_for_condition 1000 20 {
            [string match {*send-bulk-and-stream*} [migration_status 0 $task_id state]]
        } else {
            fail "ASM task did not start"
        }

        # pause the destination node to make rdb channel timeout
        pause_process $r1_pid

        # the source node will fail, the rdb child process can not
        # write data to destination, so it will timeout
        wait_for_condition 1000 30 {
            [string match {*failed*} [migration_status 0 $task_id state]] &&
            [string match {*RDB channel*Failed to send slots snapshot*} \
                [migration_status 0 $task_id last_error]]
        } else {
            fail "ASM task did not fail"
        }
        resume_process $r1_pid

        R 0 config set repl-timeout 60
        R 0 cluster migration cancel id $task_id
        R 1 cluster migration cancel id $task_id
    }

    test "Source node main channel timeout when sending incremental stream" {
        R 0 flushall
        R 0 config set repl-timeout 2   ;# 2s for main channel timeout

        set r1_pid [S 1 process_id]
        # in order to have time to pause the destination node
        R 1 config set key-load-delay 50000 ;# 50ms each 16k data

        # start migration from #0 to #1
        set task_id [setup_slot_migration_with_delay 0 1 0 100]

        # Create 200 keys of 16k size traffic on slot 0, streaming buffer need 10s (200*50ms)
        populate_slot 200 -idx 0 -slot 0 -size 16384

        # wait for streaming buffer state, then pause the destination node
        wait_for_condition 1000 20 {
            [string match {*streaming-buffer*} [migration_status 1 $task_id state]]
        } else {
            fail "ASM task did not stream buffer, state: [migration_status 1 $task_id state]"
        }
        pause_process $r1_pid

        # Start the slot 0 write load on the R 0
        set load_handle [start_write_load "127.0.0.1" [get_port 0] 100 [slot_key 0 mykey] 500]

        # the source node will fail after several seconds (including the time
        # to fill the socket buffer of source node), the main channel can not
        # write data to destination since the destination is paused
        wait_for_condition 1000 30 {
            [string match {*failed*} [migration_status 0 $task_id state]] &&
            [string match {*Main channel*Connection timeout*} \
                [migration_status 0 $task_id last_error]]
        } else {
            fail "ASM task did not fail"
        }
        stop_write_load $load_handle
        resume_process $r1_pid

        R 0 config set repl-timeout 60
        R 1 config set key-load-delay 0
        R 0 cluster migration cancel id $task_id
        R 1 cluster migration cancel id $task_id
        R 0 flushall
    }

    test "Source server paused timeout" {
        # set timeout to 0, so the task will fail immediately when checking timeout
        R 0 config set cluster-slot-migration-write-pause-timeout 0

        # start migration from node 0 to 1
        set task_id [setup_slot_migration_with_delay 0 1 0 100]

        # start the slot 0 write load on the node 0
        set slot0_key [slot_key 0 mykey]
        set load_handle [start_write_load "127.0.0.1" [get_port 0] 100 $slot0_key]

        # node 0 will fail since server paused timeout
        wait_for_condition 2000 10 {
            [string match {*failed*} [migration_status 0 $task_id state]] &&
            [string match {*Server paused timeout*} \
                [migration_status 0 $task_id last_error]]
        } else {
            fail "ASM task did not fail"
        }

        stop_write_load $load_handle

        # reset config
        R 0 config set cluster-slot-migration-write-pause-timeout 10000
        R 0 cluster migration cancel id $task_id
        R 1 cluster migration cancel id $task_id
    }

    test "Sync buffer drain timeout" {
        # set a fail point to avoid the source node to enter handoff prep state
        # to test the sync buffer drain timeout
        R 0 debug asm-failpoint "migrate-main-channel" "handoff-prep"
        R 0 config set cluster-slot-migration-sync-buffer-drain-timeout 5000

        set r1_pid [S 1 process_id]

        # start migration from node 0 to 1
        set task_id [setup_slot_migration_with_delay 0 1 0 100]

        # start the slot 0 write load on the node 0
        set slot0_key [slot_key 0 mykey]
        set load_handle [start_write_load "127.0.0.1" [get_port 0] 100 $slot0_key]

        # wait for entering streaming buffer state
        wait_for_condition 1000 10 {
            [string match {*wait-stream-eof*} [migration_status 1 $task_id state]]
        } else {
            fail "ASM task did not enter wait-stream-eof state"
        }

        pause_process $r1_pid ;# avoid the destination to apply commands

        # node 0 will fail since sync buffer drain timeout
        wait_for_condition 2000 10 {
            [string match {*failed*} [migration_status 0 $task_id state]] &&
            [string match {*Sync buffer drain timeout*} \
                [migration_status 0 $task_id last_error]]
        } else {
            fail "ASM task did not fail"
        }

        stop_write_load $load_handle
        resume_process $r1_pid

        # reset config
        R 0 config set cluster-slot-migration-sync-buffer-drain-timeout 60000
        R 0 debug asm-failpoint "" ""
        R 0 cluster migration cancel id $task_id
        R 1 cluster migration cancel id $task_id
    }

    test "Cluster implementation cannot start migrate task temporarily" {
        # Inject a fail point to make the source node not ready
        R 0 debug asm-failpoint "migrate-main-channel" "none"

        # start migration from node 0 to 1
        set task_id [R 1 CLUSTER MIGRATION IMPORT 0 100]

        # verify source node replies SYNCSLOTS with -NOTREADY
        set loglines [count_log_lines -1]
        wait_for_log_messages -1 {"*Source node replied to SYNCSLOTS SYNC with -NOTREADY, will retry later*"} $loglines 100 100

        # clear the fail point and verify the task is completed
        R 0 debug asm-failpoint "" ""
        wait_for_asm_done
        assert_equal "completed" [migration_status 0 $task_id state]
        assert_equal "completed" [migration_status 1 $task_id state]

        # cleanup
        R 0 CLUSTER MIGRATION IMPORT 0 100
        wait_for_asm_done
    }
}

start_cluster 3 3 {tags {external:skip cluster} overrides {cluster-node-timeout 60000 cluster-allow-replica-migration no}} {
    test "Test bgtrim after a successful migration" {
        R 0 debug asm-trim-method bg
        R 3 debug asm-trim-method bg
        R 0 CONFIG RESETSTAT
        R 3 CONFIG RESETSTAT

        R 0 flushall
        # Fill slot 0
        populate_slot 1000 -idx 0 -slot 0
        # Fill slot 1 with keys that have TTL
        populate_slot 1000 -idx 0 -slot 1 -prefix "expirekey" -expires 100
        # HFE key on slot 2
        set slot2_hfekey [slot_key 2 hfekey]
        R 0 HSETEX $slot2_hfekey EX 10 FIELDS 1 f1 v1

        # Fill slot 101, these keys won't be migrated
        populate_slot 1000 -idx 0 -slot 101
        # Fill slot 102 with keys that have TTL
        populate_slot 1000 -idx 0 -slot 102 -prefix "expirekey" -expires 100
        # HFE key on slot 103
        set slot103_hfekey [slot_key 103 hfekey]
        R 0 HSETEX $slot103_hfekey EX 10 FIELDS 1 f1 v1

        # migrate slot 0 to node-1
        R 1 CLUSTER MIGRATION IMPORT 0 100
        wait_for_asm_done

        # Verify the data is migrated
        wait_for_ofs_sync [Rn 0] [Rn 3]
        assert_equal 2001 [R 0 dbsize]
        assert_equal 2001 [R 3 dbsize]
        wait_for_ofs_sync [Rn 1] [Rn 4]
        assert_equal 2001 [R 1 dbsize]
        assert_equal 2001 [R 4 dbsize]

        # Verify the keys are trimmed lazily
        wait_for_condition 1000 10 {
            [S 0 lazyfreed_objects] == 2001 &&
            [S 3 lazyfreed_objects] == 2001
        } else {
            puts "lazyfreed_objects: [S 0 lazyfreed_objects] [S 3 lazyfreed_objects]"
            fail "Background trim did not happen"
        }

        # Cleanup
        R 0 CLUSTER MIGRATION IMPORT 0 100
        wait_for_asm_done
        R 0 flushall
        R 0 debug asm-trim-method default
        R 3 debug asm-trim-method default
    }

    test "Test bgtrim after a failed migration" {
        R 0 debug asm-trim-method bg
        R 3 debug asm-trim-method bg
        R 1 CONFIG RESETSTAT
        R 4 CONFIG RESETSTAT

        # Fill slot 0 on node-0 and migrate it to node-1 (with some delay)
        R 0 flushall
        set task_id [setup_slot_migration_with_delay 0 1 0 100 10000 1000]
        after 1000 ;# wait some time so that some keys are moved

        # Fail the migration
        R 1 CLUSTER MIGRATION CANCEL ID $task_id
        wait_for_asm_done

        # Verify the data is not migrated
        assert_equal 10000 [R 0 dbsize]
        assert_equal 10000 [R 3 dbsize]

        # Verify the keys are trimmed lazily after a failed import on dest side.
        wait_for_condition 1000 20 {
            [R 1 dbsize] == 0 &&
            [R 4 dbsize] == 0 &&
            [S 1 lazyfreed_objects] > 0 &&
            [S 4 lazyfreed_objects] > 0
        } else {
            fail "Background trim did not happen"
        }

        # Cleanup
        wait_for_asm_done
        R 0 flushall
        R 0 debug asm-trim-method default
        R 3 debug asm-trim-method default
    }

    test "Test bgtrim unblocks stream client" {
        # Two clients waiting for data on two different streams which are in
        # different slots. We are going to migrate one slot, which will unblock
        # the client. The other client should still be blocked.
        R 0 debug asm-trim-method bg

        set key0 [slot_key 0 mystream]
        set key1 [slot_key 1 mystream]

        # First client waits on slot-0 key
        R 0 DEL $key0
        R 0 XADD $key0 666 f v
        R 0 XGROUP CREATE $key0 mygroup $
        set rd0 [redis_deferring_client]
        $rd0 XREADGROUP GROUP mygroup Alice BLOCK 0 STREAMS $key0 ">"
        wait_for_blocked_clients_count 1

        # Second client waits on slot-1 key
        R 0 DEL $key1
        R 0 XADD $key1 666 f v
        R 0 XGROUP CREATE $key1 mygroup $
        set rd1 [redis_deferring_client]
        $rd1 XREADGROUP GROUP mygroup Alice BLOCK 0 STREAMS $key1 ">"
        wait_for_blocked_clients_count 2

        # Migrate slot 0
        R 1 CLUSTER MIGRATION IMPORT 0 0
        wait_for_asm_done

        # First client should get MOVED error
        assert_error "*MOVED*" {$rd0 read}
        $rd0 close

        # Second client should operate normally
        R 0 XADD $key1 667 f v
        set res [$rd1 read]
        assert_equal [lindex $res 0 1 0] {667-0 {f v}}
        $rd1 close

        # cleanup
        wait_for_asm_done
        R 0 CLUSTER MIGRATION IMPORT 0 0
        wait_for_asm_done
        R 0 flushall
        R 0 debug asm-trim-method default
    }

    test "Test bgtrim touches watched keys" {
        R 0 debug asm-trim-method bg

        # bgtrim should touch watched keys on migrated slots
        set key0 [slot_key 0 key]
        R 0 set $key0 30
        R 0 watch $key0
        R 1 CLUSTER MIGRATION IMPORT 0 0
        wait_for_asm_done
        R 0 multi
        R 0 ping
        assert_equal {} [R 0 exec]

        # bgtrim should not touch watched keys on other slots
        set key2 [slot_key 2 key]
        R 0 set $key2 30
        R 0 watch $key2
        R 1 CLUSTER MIGRATION IMPORT 1 1
        wait_for_asm_done
        R 0 multi
        R 0 ping
        assert_equal PONG [R 0 exec]

        # cleanup
        wait_for_asm_done
        R 0 CLUSTER MIGRATION IMPORT 0 1
        wait_for_asm_done
        R 0 flushall
        R 0 debug asm-trim-method default
    }

    test "Test bgtrim after a FAILOVER on destination side" {
        R 1 debug asm-trim-method bg
        R 4 debug asm-trim-method bg

        set loglines [count_log_lines -4]

        # Fill slot 0 on node-0 and migrate it to node-1 (with some delay)
        R 0 flushall
        set task_id [setup_slot_migration_with_delay 0 1 0 100 10000 1000]
        after 1000 ;# wait some time so that some keys are moved

        # Trigger a failover with force to simulate unreachable master and
        # verify unowned keys are trimmed once replica becomes master.
        failover_and_wait_for_done 4 force
        wait_for_log_messages -4 {"*Detected keys in slots that do not belong*Scheduling trim*"} $loglines 1000 10
        wait_for_condition 1000 10 {
            [R 1 dbsize] == 0 &&
            [R 4 dbsize] == 0
        } else {
            fail "Background trim did not happen"
        }

        # cleanup
        wait_for_cluster_propagation
        failover_and_wait_for_done 1
        R 0 config set rdb-key-save-delay 0
        R 1 debug asm-trim-method default
        R 4 debug asm-trim-method default
        wait_for_asm_done
    }

    test "CLUSTER SETSLOT is not allowed if there is a pending trim job" {
        R 0 debug asm-trim-method bg
        R 3 debug asm-trim-method bg

        # Fill slot 0 on node-0 and migrate it to node-1 (with some delay)
        R 0 flushall
        set task_id [setup_slot_migration_with_delay 0 1 0 100 10000 1000]

        # Pause will cancel the task and there will be a pending trim job
        # until writes are allowed again.
        R 1 client pause 100000 write ;# pause 100s
        wait_for_asm_done

        # CLUSTER SETSLOT is not allowed if there is a pending trim job.
        assert_error {*There is a pending trim job for slot 0*} {R 1 CLUSTER SETSLOT 0 STABLE}

        # Unpause the server, trim will be triggered and SETSLOT will be allowed
        R 1 client unpause
        R 1 CLUSTER SETSLOT 0 STABLE
    }
}

start_cluster 3 3 {tags {external:skip cluster} overrides {cluster-node-timeout 60000 cluster-allow-replica-migration no save ""}} {
    test "Test active trim after a successful migration" {
        R 0 debug asm-trim-method active
        R 3 debug asm-trim-method active
        populate_slot 500 -slot 0
        populate_slot 500 -slot 1
        populate_slot 500 -slot 3
        populate_slot 500 -slot 4

        # Migrate 1500 keys
        R 1 CLUSTER MIGRATION IMPORT 0 1 3 3
        wait_for_asm_done

        wait_for_condition 1000 10 {
            [CI 0 cluster_slot_migration_active_tasks] == 0 &&
            [CI 0 cluster_slot_migration_active_trim_running] == 0 &&
            [CI 0 cluster_slot_migration_active_trim_current_job_trimmed] == 1500 &&
            [CI 3 cluster_slot_migration_active_trim_running] == 0 &&
            [CI 3 cluster_slot_migration_active_trim_current_job_trimmed] == 1500
        } else {
            fail "trim failed"
        }

        assert_equal 1500 [CI 0 cluster_slot_migration_active_trim_current_job_keys]
        assert_equal 1500 [CI 3 cluster_slot_migration_active_trim_current_job_keys]

        assert_equal 500 [R 0 dbsize]
        assert_equal 500 [R 3 dbsize]
        assert_equal 1500 [R 1 dbsize]
        assert_equal 1500 [R 4 dbsize]
        assert_equal 0 [R 0 cluster countkeysinslot 0]
        assert_equal 0 [R 0 cluster countkeysinslot 1]
        assert_equal 0 [R 0 cluster countkeysinslot 3]
        assert_equal 500 [R 0 cluster countkeysinslot 4]

        # cleanup
        R 0 debug asm-trim-method default
        R 3 debug asm-trim-method default
        R 0 CLUSTER MIGRATION IMPORT 0 1 3 3
        wait_for_asm_done
        R 0 flushall
        R 1 flushall
    }

    test "Test multiple active trim jobs can be scheduled" {
        # Active trim will be scheduled but it won't run
        R 0 debug asm-trim-method active -1
        R 3 debug asm-trim-method active -1

        populate_slot 500 -slot 0
        populate_slot 500 -slot 1
        populate_slot 500 -slot 3
        populate_slot 500 -slot 4

        # Migrate 1500 keys
        R 1 CLUSTER MIGRATION IMPORT 0 1
        wait_for_condition 1000 10 {
            [CI 0 cluster_slot_migration_active_tasks] == 0 &&
            [CI 0 cluster_slot_migration_active_trim_running] == 1 &&
            [CI 3 cluster_slot_migration_active_trim_running] == 1
        } else {
            fail "migrate failed"
        }

        # Migrate another slot and verify there are two trim tasks on the source
        R 1 CLUSTER MIGRATION IMPORT 3 3
        wait_for_condition 1000 10 {
            [CI 0 cluster_slot_migration_active_tasks] == 0 &&
            [CI 0 cluster_slot_migration_active_trim_running] == 2 &&
            [CI 3 cluster_slot_migration_active_trim_running] == 2
        } else {
            fail "migrate failed"
        }

        # Enabled active trim and wait until it is completed.
        R 0 debug asm-trim-method active 0
        R 3 debug asm-trim-method active 0
        wait_for_asm_done

        assert_equal 500 [R 0 dbsize]
        assert_equal 500 [R 3 dbsize]
        assert_equal 0 [R 0 cluster countkeysinslot 0]
        assert_equal 0 [R 0 cluster countkeysinslot 1]
        assert_equal 0 [R 0 cluster countkeysinslot 3]
        assert_equal 500 [R 0 cluster countkeysinslot 4]

        # cleanup
        R 0 debug asm-trim-method default
        R 3 debug asm-trim-method default
        R 0 CLUSTER MIGRATION IMPORT 0 1 3 3
        wait_for_asm_done
        R 0 flushall
        R 1 flushall
    }

    test "Test active-trim clears partially imported keys on cancel" {
        R 1 debug asm-trim-method active
        R 4 debug asm-trim-method active

        # Rdb delivery will take 10 seconds
        R 0 config set rdb-key-save-delay 10000
        populate_slot 250 -slot 0
        populate_slot 250 -slot 1
        populate_slot 250 -slot 3
        populate_slot 250 -slot 4

        R 1 CLUSTER MIGRATION IMPORT 0 100
        after 2000
        R 1 CLUSTER MIGRATION CANCEL ALL
        wait_for_asm_done

        assert_morethan [CI 1 cluster_slot_migration_active_trim_current_job_keys] 0
        assert_morethan [CI 4 cluster_slot_migration_active_trim_current_job_trimmed] 0

        assert_equal 1000 [R 0 dbsize]
        assert_equal 1000 [R 3 dbsize]
        assert_equal 0 [R 1 dbsize]
        assert_equal 0 [R 4 dbsize]

        # Cleanup
        R 1 debug asm-trim-method default
        R 4 debug asm-trim-method default
        R 0 config set rdb-key-save-delay 0
    }

    test "Test active-trim clears partially imported keys on failover" {
        R 1 debug asm-trim-method active
        R 4 debug asm-trim-method active

        # Rdb delivery will take 10 seconds
        R 0 config set rdb-key-save-delay 10000

        populate_slot 250 -slot 0
        populate_slot 250 -slot 1
        populate_slot 250 -slot 3
        populate_slot 250 -slot 4

        set prev_trim_started_1 [CI 1 cluster_slot_migration_stats_active_trim_started]
        set prev_trim_started_4 [CI 4 cluster_slot_migration_stats_active_trim_started]

        R 1 CLUSTER MIGRATION IMPORT 0 100
        after 2000
        failover_and_wait_for_done 4
        wait_for_asm_done

        # Verify there is at least one trim job started
        assert_morethan [CI 1 cluster_slot_migration_stats_active_trim_started] $prev_trim_started_1
        assert_morethan [CI 4 cluster_slot_migration_stats_active_trim_started] $prev_trim_started_4

        assert_equal 1000 [R 0 dbsize]
        assert_equal 1000 [R 3 dbsize]
        assert_equal 0 [R 1 dbsize]
        assert_equal 0 [R 4 dbsize]

        # Cleanup
        failover_and_wait_for_done 1
        R 1 debug asm-trim-method default
        R 4 debug asm-trim-method default
        R 0 config set rdb-key-save-delay 0
        R 0 flushall
        R 1 flushall
    }

    test "Test import task does not start if active trim is in progress for the same slots" {
        # Active trim will be scheduled but it won't run
        R 0 flushall
        R 1 flushall
        R 0 debug asm-trim-method active -1

        populate_slot 500 -slot 0
        populate_slot 500 -slot 1

        # Migrate 1000 keys
        R 1 CLUSTER MIGRATION IMPORT 0 1
        wait_for_condition 1000 10 {
            [CI 0 cluster_slot_migration_active_tasks] == 0 &&
            [CI 0 cluster_slot_migration_active_trim_running] == 1
        } else {
            fail "migrate failed"
        }

        # Try to migrate slots back
        R 0 CLUSTER MIGRATION IMPORT 0 1
        wait_for_log_messages 0 {"*Can not start import task*trim in progress for some of the slots*"} 0 1000 10

        # Enabled active trim and verify slots are imported back
        R 0 debug asm-trim-method active 0
        wait_for_asm_done

        assert_equal 1000 [R 0 dbsize]
        assert_equal 500 [R 0 cluster countkeysinslot 0]
        assert_equal 500 [R 0 cluster countkeysinslot 1]

        # cleanup
        R 0 debug asm-trim-method default
        R 0 flushall
    }

    test "Rdb save during active trim should skip keys in trimmed slots" {
        # Insert some delay to activate trim
        R 0 debug asm-trim-method active 1000
        R 0 config set repl-diskless-sync-delay 0
        R 0 flushall

        populate_slot 5000 -idx 0 -slot 0
        populate_slot 5000 -idx 0 -slot 1
        populate_slot 5000 -idx 0 -slot 2

        # Start migration and wait until trim is in progress
        R 1 CLUSTER MIGRATION IMPORT 0 1
        wait_for_condition 1000 10 {
            [CI 0 cluster_slot_migration_active_tasks] == 0 &&
            [CI 0 cluster_slot_migration_active_trim_running] == 1 &&
            [S 0 rdb_bgsave_in_progress] == 0
        } else {
            puts "[CI 0 cluster_slot_migration_active_tasks]"
            puts "[CI 0 cluster_slot_migration_active_trim_running]"
            fail "trim failed"
        }

        # Trigger save during active trim
        R 0 save
        # Wait until the log contains a "keys skipped" message with a non-zero value
        wait_for_log_messages 0 {"*BGSAVE done, 5000 keys saved, [1-9]* keys skipped*"} 0 1000 10

        restart_server 0 yes no yes nosave
        assert_equal 5000 [R 0 dbsize]
        assert_equal 0 [R 0 cluster countkeysinslot 0]
        assert_equal 0 [R 0 cluster countkeysinslot 1]
        assert_equal 5000 [R 0 cluster countkeysinslot 2]

        # Cleanup
        wait_for_cluster_propagation
        wait_for_cluster_state "ok"
        R 0 flushall
        R 1 flushall
        R 0 save
        R 0 CLUSTER MIGRATION IMPORT 0 1
        wait_for_asm_done
    }

    test "AOF rewrite during active trim should skip keys in trimmed slots" {
        R 0 debug asm-trim-method active 1000
        R 0 config set repl-diskless-sync-delay 0
        R 0 config set aof-use-rdb-preamble no
        R 0 config set appendonly yes
        R 0 config rewrite
        R 0 flushall
        populate_slot 5000 -idx 0 -slot 0
        populate_slot 5000 -idx 0 -slot 1
        populate_slot 5000 -idx 0 -slot 2

        R 1 CLUSTER MIGRATION IMPORT 0 1
        wait_for_condition 1000 10 {
            [CI 0 cluster_slot_migration_active_tasks] == 0 &&
            [CI 0 cluster_slot_migration_active_trim_running] == 1
        } else {
            puts "[CI 0 cluster_slot_migration_active_tasks]"
            puts "[CI 0 cluster_slot_migration_active_trim_running]"
            fail "trim failed"
        }

        wait_for_condition 50 100 {
            [S 0 rdb_bgsave_in_progress] == 0
        } else {
            fail "bgsave is in progress"
        }

        R 0 bgrewriteaof
        # Wait until the log contains a "keys skipped" message with a non-zero value
        wait_for_log_messages 0 {"*AOF rewrite done, [1-9]* keys saved, [1-9]* keys skipped*"} 0 1000 10

        restart_server 0 yes no yes nosave
        assert_equal 5000 [R 0 dbsize]
        assert_equal 0 [R 0 cluster countkeysinslot 0]
        assert_equal 0 [R 0 cluster countkeysinslot 1]
        assert_equal 5000 [R 0 cluster countkeysinslot 2]

        # cleanup
        R 0 config set appendonly no
        R 0 config rewrite
        restart_server 0 yes no yes nosave
        wait_for_cluster_propagation
        wait_for_cluster_state "ok"
        R 0 flushall
        R 1 flushall
        R 0 save
        R 0 CLUSTER MIGRATION IMPORT 0 1
        wait_for_asm_done
    }

    test "Pause actions will stop active trimming" {
        R 0 debug asm-trim-method active 1000
        R 0 config set repl-diskless-sync-delay 0
        R 0 flushall
        populate_slot 10000 -idx 0 -slot 0

        R 1 CLUSTER MIGRATION IMPORT 0 100
        wait_for_condition 1000 10 {
            [CI 0 cluster_slot_migration_active_tasks] == 0 &&
            [CI 0 cluster_slot_migration_active_trim_running] == 1
        } else {
            puts "[CI 0 cluster_slot_migration_active_tasks]"
            puts "[CI 0 cluster_slot_migration_active_trim_running]"
            fail "trim failed"
        }

        # Pause the server and verify no keys are trimmed
        R 0 client pause 100000 write ;# pause 100s
        set prev [CI 0 cluster_slot_migration_active_trim_current_job_trimmed]
        after 1000 ; # wait some time to see if any keys are trimmed
        set curr [CI 0 cluster_slot_migration_active_trim_current_job_trimmed]
        assert_equal $prev $curr

        R 0 client unpause
        R 0 debug asm-trim-method default
        wait_for_asm_done
        assert_equal 0 [R 0 dbsize]

        # revert
        R 0 CLUSTER MIGRATION IMPORT 0 100
        wait_for_asm_done
        assert_equal 10000 [R 0 dbsize]
    }

    foreach diskless_load {"disabled" "swapdb" "on-empty-db"} {
        test "Test fullsync cancels active trim (repl-diskless-load $diskless_load)" {
            R 3 debug asm-trim-method active -10
            R 3 config set repl-diskless-load $diskless_load
            R 0 flushall

            R 0 config set repl-diskless-sync-delay 0
            populate_slot 10000 -idx 0 -slot 0

            R 1 CLUSTER MIGRATION IMPORT 0 0
            wait_for_condition 1000 10 {
                [CI 0 cluster_slot_migration_active_tasks] == 0 &&
                [CI 0 cluster_slot_migration_active_trim_running] == 0 &&
                [CI 3 cluster_slot_migration_active_trim_running] == 1
            } else {
                puts "[CI 0 cluster_slot_migration_active_tasks]"
                puts "[CI 0 cluster_slot_migration_active_trim_running]"
                puts "[CI 3 cluster_slot_migration_active_trim_running]"
                fail "trim failed"
            }

            set prev_cancelled [CI 3 cluster_slot_migration_stats_active_trim_cancelled]
            R 0 config set client-output-buffer-limit "replica 1024 0 0"

            # Trigger a fullsync
            populate_slot 1 -idx 0 -size 2000000 -slot 2

            wait_for_condition 1000 10 {
                [CI 3 cluster_slot_migration_active_trim_running] == 0 &&
                [CI 3 cluster_slot_migration_stats_active_trim_cancelled] == $prev_cancelled + 1
            } else {
                puts "[CI 3 cluster_slot_migration_active_trim_running]"
                puts "[CI 3 cluster_slot_migration_stats_active_trim_cancelled]"
                fail "trim failed"
            }

            R 3 debug asm-trim-method active 0
            R 3 config set repl-diskless-load disabled
            R 0 CLUSTER MIGRATION IMPORT 0 0
            wait_for_asm_done
            wait_for_ofs_sync [Rn 0] [Rn 3]
            assert_equal 10001 [R 0 dbsize]
            assert_equal 10001 [R 3 dbsize]
            assert_equal 0 [R 1 dbsize]
            assert_equal 0 [R 4 dbsize]
            R 0 flushall
        }
    }

    test "Test importing slots while active-trim is in progress for the same slots on replica" {
       R 3 debug asm-trim-method active 10000
       R 0 flushall
       populate_slot 10000 -slot 0
       wait_for_ofs_sync [Rn 0] [Rn 3]

       # Wait until active trim is in progress on replica
       R 1 CLUSTER MIGRATION IMPORT 0 100
       wait_for_condition 1000 10 {
           [CI 0 cluster_slot_migration_active_tasks] == 0 &&
           [CI 0 cluster_slot_migration_active_trim_running] == 0 &&
           [CI 3 cluster_slot_migration_active_trim_running] == 1
       } else {
           puts "[CI 0 cluster_slot_migration_active_tasks]"
           puts "[CI 0 cluster_slot_migration_active_trim_running]"
           puts "[CI 3 cluster_slot_migration_active_trim_running]"
           fail "trim failed"
       }

       set loglines [count_log_lines -3]

       # Get slots back
       R 0 CLUSTER MIGRATION IMPORT 0 100
       wait_for_condition 1000 20 {
           [CI 0 cluster_slot_migration_active_tasks] == 1 &&
           [CI 0 cluster_slot_migration_active_trim_running] == 0 &&
           [CI 3 cluster_slot_migration_active_trim_running] == 1
       } else {
           fail "trim failed"
       }

       # Verify replica blocks master until trim is done
       wait_for_log_messages -3 {"*Blocking master client until trim job is done*"} $loglines 1000 30
       R 3 debug asm-trim-method active 0
       wait_for_log_messages -3 {"*Unblocking master client after active trim*"} $loglines 1000 30

       wait_for_asm_done
       wait_for_ofs_sync [Rn 0] [Rn 3]
       assert_equal 10000 [R 0 dbsize]
       assert_equal 10000 [R 3 dbsize]
       assert_equal 0 [R 1 dbsize]
       assert_equal 0 [R 4 dbsize]
    }

    test "TRIMSLOTS should not trim slots that this node is serving" {
        assert_error {*the slot 0 is served by this node*} {R 0 trimslots ranges 1 0 0}
        assert_error {*READONLY*} {R 3 trimslots ranges 1 0 100}
        assert_equal {OK} [R 0 trimslots ranges 1 16383 16383]
        assert_error {*READONLY*} {R 3 trimslots ranges 1 16383 16383}
    }

    test "Trigger multiple active trim jobs at the same time" {
        R 1 debug asm-trim-method active 0
        R 1 flushall

        set prev_trim_done [CI 1 cluster_slot_migration_stats_active_trim_completed]

        R 1 debug populate 1000 [slot_prefix 0] 100
        R 1 debug populate 1000 [slot_prefix 1] 100
        R 1 debug populate 1000 [slot_prefix 2] 100

        R 1 multi
        R 1 trimslots ranges 1 0 0
        R 1 trimslots ranges 1 1 1
        R 1 trimslots ranges 1 2 2
        R 1 exec

        wait_for_condition 1000 10 {
            [CI 1 cluster_slot_migration_stats_active_trim_completed] == $prev_trim_done + 3
        } else {
            fail "active trim failed"
        }

        R 1 flushall
        R 1 debug asm-trim-method default
    }

    test "Restart will clean up unowned slot keys" {
        R 1 flushall

        # generate 1000 keys belonging to slot 0
        R 1 debug populate 1000 [slot_prefix 0] 100
        assert {[scan [regexp -inline {keys\=([\d]*)} [R 1 info keyspace]] keys=%d] >= 1000}

        # restart node-1
        restart_server -1 true false true save
        wait_for_cluster_propagation
        wait_for_cluster_state "ok"

        # Node-1 has no keys since unowned slot 0 keys were cleaned up during restart
        assert {[scan [regexp -inline {keys\=([\d]*)} [R 1 info keyspace]] keys=%d] == {}}

        R 1 flushall
    }

    test "Test active trim is used when client tracking is used" {
        R 0 flushall
        R 1 flushall
        R 0 debug asm-trim-method default
        R 1 debug asm-trim-method default

        set prev_active_trim [CI 0 cluster_slot_migration_stats_active_trim_completed]

        # Setup a tracking client that is redirected to a pubsub client
        set rd_redirection [redis_deferring_client]
        $rd_redirection client id
        set redir_id [$rd_redirection read]
        $rd_redirection subscribe __redis__:invalidate
        $rd_redirection read ; # Consume the SUBSCRIBE reply.

        # setup tracking
        set key0 [slot_key 0 key]
        R 0 CLIENT TRACKING on REDIRECT $redir_id
        R 0 SET $key0 1
        R 0 GET $key0
        R 1 CLUSTER MIGRATION IMPORT 0 0
        wait_for_asm_done

        wait_for_condition 1000 10 {
            [CI 0 cluster_slot_migration_stats_active_trim_completed] == [expr $prev_active_trim + 1]
        } else {
            fail "active trim did not happen"
        }

        # Verify the tracking client received the invalidation message
        set msg [$rd_redirection read]
        set head [lindex $msg 0]

        if {$head eq "message"} {
            # RESP 2
            set got_key [lindex [lindex $msg 2] 0]
        } elseif {$head eq "invalidate"} {
            # RESP 3
            set got_key [lindex $msg 1 0]
        } else {
            fail "unexpected invalidation message: $msg"
        }
        assert_equal $got_key $key0

        # cleanup
        $rd_redirection close
        wait_for_asm_done
        R 0 CLUSTER MIGRATION IMPORT 0 0
        wait_for_asm_done
        R 0 flushall
    }
}

set testmodule [file normalize tests/modules/atomicslotmigration.so]

start_cluster 3 6 [list tags {external:skip cluster modules} config_lines [list loadmodule $testmodule cluster-node-timeout 60000 cluster-allow-replica-migration no]] {
    test "Module api sanity" {
        R 0 asm.sanity ;# on master
        R 3 asm.sanity ;# on replica
    }

    test "Module replicate cross slot command" {
        set task_id [setup_slot_migration_with_delay 0 1 0 100]
        set listkey [slot_key 0 "asmlist"]
        # replicate cross slot command during migrating
        R 0 asm.lpush_replicate_crossslot_command $listkey "item1"

        # node 0 will fail due to cross slot
        wait_for_condition 2000 10 {
            [string match {*canceled*} [migration_status 0 $task_id state]] &&
            [string match {*cross slot*} [migration_status 0 $task_id last_error]]
        } else {
            fail "ASM task did not fail"
        }
        R 1 CLUSTER MIGRATION CANCEL ID $task_id

        # sanity check if lpush replicated correctly to the replica
        wait_for_ofs_sync [Rn 0] [Rn 3]
        assert_equal {item1} [R 0 lrange $listkey 0 -1]
        R 3 readonly
        assert_equal {item1} [R 3 lrange $listkey 0 -1]
    }

    test "Test RM_ClusterCanAccessKeysInSlot" {
        # Test invalid slots
        assert_equal 0 [R 0 asm.cluster_can_access_keys_in_slot -1]
        assert_equal 0 [R 0 asm.cluster_can_access_keys_in_slot 20000]
        assert_equal 0 [R 2 asm.cluster_can_access_keys_in_slot 16384]
        assert_equal 0 [R 5 asm.cluster_can_access_keys_in_slot 16384]

        # Test on a master-replica pair
        assert_equal 1 [R 0 asm.cluster_can_access_keys_in_slot 0]
        assert_equal 1 [R 0 asm.cluster_can_access_keys_in_slot 100]
        assert_equal 1 [R 3 asm.cluster_can_access_keys_in_slot 0]
        assert_equal 1 [R 3 asm.cluster_can_access_keys_in_slot 100]

        # Test on a master-replica pair
        assert_equal 1 [R 2 asm.cluster_can_access_keys_in_slot 16383]
        assert_equal 1 [R 5 asm.cluster_can_access_keys_in_slot 16383]
    }

    test "Test RM_ClusterCanAccessKeysInSlot returns false for unowned slots" {
        # Active trim will be scheduled but it won't run
        R 0 debug asm-trim-method active -1
        R 3 debug asm-trim-method active -1

        setup_slot_migration_with_delay 0 1 0 100 3 1000000

        # Verify importing slots are not local
        assert_equal 0 [R 1 asm.cluster_can_access_keys_in_slot 0]
        assert_equal 0 [R 1 asm.cluster_can_access_keys_in_slot 100]
        assert_equal 0 [R 4 asm.cluster_can_access_keys_in_slot 0]
        assert_equal 0 [R 4 asm.cluster_can_access_keys_in_slot 100]

        wait_for_condition 1000 10 {
            [CI 0 cluster_slot_migration_active_tasks] == 0 &&
            [CI 0 cluster_slot_migration_active_trim_running] == 1 &&
            [CI 3 cluster_slot_migration_active_trim_running] == 1
        } else {
            fail "migrate failed"
        }

        # Wait for config propagation before checking the slot ownership on replica
        wait_for_cluster_propagation

        # Verify slots that are being trimmed are not local
        assert_equal 0 [R 0 asm.cluster_can_access_keys_in_slot 0]
        assert_equal 0 [R 0 asm.cluster_can_access_keys_in_slot 100]
        assert_equal 0 [R 3 asm.cluster_can_access_keys_in_slot 0]
        assert_equal 0 [R 3 asm.cluster_can_access_keys_in_slot 100]

        # Enabled active trim and wait until it is completed.
        R 0 debug asm-trim-method active 0
        R 3 debug asm-trim-method active 0
        wait_for_asm_done
        wait_for_ofs_sync [Rn 0] [Rn 3]

        # Verify slots are local after migration
        assert_equal 1 [R 1 asm.cluster_can_access_keys_in_slot 0]
        assert_equal 1 [R 1 asm.cluster_can_access_keys_in_slot 100]
        assert_equal 1 [R 4 asm.cluster_can_access_keys_in_slot 0]
        assert_equal 1 [R 4 asm.cluster_can_access_keys_in_slot 100]

        # cleanup
        R 0 debug asm-trim-method default
        R 3 debug asm-trim-method default
        R 0 CLUSTER MIGRATION IMPORT 0 100
        wait_for_asm_done
        R 0 flushall
        R 1 flushall
    }

    foreach trim_method {"active" "bg"} {
        test "Test cluster module notifications on a successful migration ($trim_method-trim)" {
            clear_module_event_log
            R 0 debug asm-trim-method $trim_method
            R 3 debug asm-trim-method $trim_method
            R 6 debug asm-trim-method $trim_method

            # Set a key in the slot range
            set key [slot_key 0 mykey]
            R 0 set $key "value"

            # Migrate the slot ranges
            set task_id [R 1 CLUSTER MIGRATION IMPORT 0 100 200 300]
            wait_for_asm_done

            set src_id [R 0 cluster myid]
            set dest_id [R 1 cluster myid]

            # Verify the events on source, both master and replica
            set migrate_event_log [list \
                "sub: cluster-slot-migration-migrate-started, source_node_id:$src_id, destination_node_id:$dest_id, task_id:$task_id, slots:0-100,200-300" \
                "sub: cluster-slot-migration-migrate-completed, source_node_id:$src_id, destination_node_id:$dest_id, task_id:$task_id, slots:0-100,200-300" \
            ]
            assert_equal [R 0 asm.get_cluster_event_log] $migrate_event_log
            assert_equal [R 3 asm.get_cluster_event_log] {}
            assert_equal [R 6 asm.get_cluster_event_log] {}

            # Verify the events on destination, both master and replica
            set import_event_log [list \
                "sub: cluster-slot-migration-import-started, source_node_id:$src_id, destination_node_id:$dest_id, task_id:$task_id, slots:0-100,200-300" \
                "sub: cluster-slot-migration-import-completed, source_node_id:$src_id, destination_node_id:$dest_id, task_id:$task_id, slots:0-100,200-300" \
            ]
            wait_for_condition 500 20 {
                [R 1 asm.get_cluster_event_log] eq $import_event_log &&
                [R 4 asm.get_cluster_event_log] eq $import_event_log &&
                [R 7 asm.get_cluster_event_log] eq $import_event_log
            } else {
                puts "R1: [R 1 asm.get_cluster_event_log]"
                puts "R4: [R 4 asm.get_cluster_event_log]"
                puts "R7: [R 7 asm.get_cluster_event_log]"
                fail "ASM import event not received"
            }

            # Verify the trim events
            if {$trim_method eq "active"} {
                set trim_event_log [list \
                    "sub: cluster-slot-migration-trim-started, slots:0-100,200-300" \
                    "keyspace: key_trimmed, key: $key" \
                    "sub: cluster-slot-migration-trim-completed, slots:0-100,200-300" \
                ]
            } else {
                set trim_event_log [list \
                    "sub: cluster-slot-migration-trim-background, slots:0-100,200-300" \
                ]
            }
            wait_for_condition 500 10 {
                [R 0 asm.get_cluster_trim_event_log] eq $trim_event_log &&
                [R 3 asm.get_cluster_trim_event_log] eq $trim_event_log &&
                [R 6 asm.get_cluster_trim_event_log] eq $trim_event_log
            } else {
                fail "ASM source trim event not received"
            }

            # cleanup
            R 0 CLUSTER MIGRATION IMPORT 0 100 200 300
            wait_for_asm_done
            clear_module_event_log
            reset_default_trim_method
            R 0 flushall
            R 1 flushall
        }

        test "Test cluster module notifications on a failed migration ($trim_method-trim)" {
            clear_module_event_log
            R 1 debug asm-trim-method $trim_method
            R 4 debug asm-trim-method $trim_method
            R 7 debug asm-trim-method $trim_method

            # Set a key in the slot range
            set key [slot_key 0 mykey]
            R 0 set $key "value"

            # Start migration and cancel it
            set task_id [setup_slot_migration_with_delay 0 1 0 100 0 2000000]
            # Wait until at least one key is moved to destination
            wait_for_condition 1000 10 {
                [scan [regexp -inline {keys\=([\d]*)} [R 1 info keyspace]] keys=%d] >= 1
            } else {
                fail "Key not moved to destination"
            }
            R 1 CLUSTER MIGRATION CANCEL ID $task_id
            wait_for_asm_done

            set src_id [R 0 cluster myid]
            set dest_id [R 1 cluster myid]

            # Verify the events on source, both master and replica
            set migrate_event_log [list \
                "sub: cluster-slot-migration-migrate-started, source_node_id:$src_id, destination_node_id:$dest_id, task_id:$task_id, slots:0-100" \
                "sub: cluster-slot-migration-migrate-failed, source_node_id:$src_id, destination_node_id:$dest_id, task_id:$task_id, slots:0-100" \
            ]
            assert_equal [R 0 asm.get_cluster_event_log] $migrate_event_log
            assert_equal [R 3 asm.get_cluster_event_log] {}
            assert_equal [R 6 asm.get_cluster_event_log] {}

            # Verify the events on destination, both master and replica
            set import_event_log [list \
                "sub: cluster-slot-migration-import-started, source_node_id:$src_id, destination_node_id:$dest_id, task_id:$task_id, slots:0-100" \
                "sub: cluster-slot-migration-import-failed, source_node_id:$src_id, destination_node_id:$dest_id, task_id:$task_id, slots:0-100" \
            ]
            wait_for_condition 500 10 {
                [R 1 asm.get_cluster_event_log] eq $import_event_log &&
                [R 4 asm.get_cluster_event_log] eq $import_event_log &&
                [R 7 asm.get_cluster_event_log] eq $import_event_log
            } else {
                fail "ASM import event not received"
            }

            # Verify the trim events on destination (partially imported keys are trimmed)
            if {$trim_method eq "active"} {
                set trim_event_log [list \
                    "sub: cluster-slot-migration-trim-started, slots:0-100" \
                    "keyspace: key_trimmed, key: $key" \
                    "sub: cluster-slot-migration-trim-completed, slots:0-100" \
                ]
            } else {
                set trim_event_log [list \
                    "sub: cluster-slot-migration-trim-background, slots:0-100" \
                ]
            }
            wait_for_condition 500 10 {
                [R 1 asm.get_cluster_trim_event_log] eq $trim_event_log &&
                [R 4 asm.get_cluster_trim_event_log] eq $trim_event_log &&
                [R 7 asm.get_cluster_trim_event_log] eq $trim_event_log
            } else {
                fail "ASM destination trim event not received"
            }

            # cleanup
            clear_module_event_log
            reset_default_trim_method
            wait_for_asm_done
            R 0 flushall
            R 1 flushall
        }

        test "Test cluster module notifications on failover ($trim_method-trim)" {
            # NOTE: cluster legacy may have a bug, multiple manual failover will fail,
            # so only perform one round of failover test, fix it later
            if {$trim_method eq "bg"} {
            clear_module_event_log
            R 1 debug asm-trim-method $trim_method
            R 4 debug asm-trim-method $trim_method
            R 7 debug asm-trim-method $trim_method

            # Set a key in the slot range
            set key [slot_key 0 mykey]
            R 0 set $key "value"

            # Start migration
            set task_id [setup_slot_migration_with_delay 0 1 0 100 0 2000000]
            # Wait until at least one key is moved to destination
            wait_for_condition 1000 10 {
                [scan [regexp -inline {keys\=([\d]*)} [R 1 info keyspace]] keys=%d] >= 1
            } else {
                fail "Key not moved to destination"
            }

            failover_and_wait_for_done 4
            wait_for_asm_done

            set src_id [R 0 cluster myid]
            set dest_id [R 1 cluster myid]

            # Verify the events on source, both master and replica
            set migrate_event_log [list \
                "sub: cluster-slot-migration-migrate-started, source_node_id:$src_id, destination_node_id:$dest_id, task_id:$task_id, slots:0-100" \
                "sub: cluster-slot-migration-migrate-failed, source_node_id:$src_id, destination_node_id:$dest_id, task_id:$task_id, slots:0-100" \
            ]
            assert_equal [R 0 asm.get_cluster_event_log] $migrate_event_log
            assert_equal [R 3 asm.get_cluster_event_log] {}
            assert_equal [R 6 asm.get_cluster_event_log] {}

            # Verify the events on destination, both master and replica
            set import_event_log [list \
                "sub: cluster-slot-migration-import-started, source_node_id:$src_id, destination_node_id:$dest_id, task_id:$task_id, slots:0-100" \
                "sub: cluster-slot-migration-import-failed, source_node_id:$src_id, destination_node_id:$dest_id, task_id:$task_id, slots:0-100" \
            ]
            wait_for_condition 500 20 {
                [R 1 asm.get_cluster_event_log] eq $import_event_log &&
                [R 4 asm.get_cluster_event_log] eq $import_event_log &&
                [R 7 asm.get_cluster_event_log] eq $import_event_log
            } else {
                puts "R1: [R 1 asm.get_cluster_event_log]"
                puts "R4: [R 4 asm.get_cluster_event_log]"
                puts "R7: [R 7 asm.get_cluster_event_log]"
                fail "ASM import event not received"
            }

            # Verify the trim events on destination (partially imported keys are trimmed)
            # NOTE: after failover, the new master will initiate the slot trimming,
            # and only slot 0 has data, so only slot 0 is trimmed
            if {$trim_method eq "active"} {
                set trim_event_log [list \
                    "sub: cluster-slot-migration-trim-started, slots:0-0" \
                    "keyspace: key_trimmed, key: $key" \
                    "sub: cluster-slot-migration-trim-completed, slots:0-0" \
                ]
            } else {
                set trim_event_log [list \
                    "sub: cluster-slot-migration-trim-background, slots:0-0" \
                ]
            }
            wait_for_condition 500 20 {
                [R 1 asm.get_cluster_trim_event_log] eq $trim_event_log &&
                [R 4 asm.get_cluster_trim_event_log] eq $trim_event_log &&
                [R 7 asm.get_cluster_trim_event_log] eq $trim_event_log
            } else {
                puts "R1: [R 1 asm.get_cluster_trim_event_log]"
                puts "R4: [R 4 asm.get_cluster_trim_event_log]"
                puts "R7: [R 7 asm.get_cluster_trim_event_log]"
                fail "ASM destination trim event not received"
            }

            # cleanup
            failover_and_wait_for_done 1
            clear_module_event_log
            reset_default_trim_method
            R 0 flushall
            R 1 flushall
        }
        }
    }

    foreach with_rdb {"with" "without"} {
        test "Test cluster module notifications when replica restart $with_rdb RDB during importing" {
            clear_module_event_log
            R 1 debug asm-trim-method $trim_method
            R 4 debug asm-trim-method $trim_method
            R 7 debug asm-trim-method $trim_method
            R 4 config set save ""

            set src_id [R 0 cluster myid]
            set dest_id [R 1 cluster myid]

            # Set a key in the slot range
            set key [slot_key 0 mykey]
            R 0 set $key "value"

            # Start migration, 2s delay
            set task_id [setup_slot_migration_with_delay 0 1 0 100 0 2000000]
            # Wait until at least one key is moved to destination
            wait_for_condition 1000 10 {
                [scan [regexp -inline {keys\=([\d]*)} [R 1 info keyspace]] keys=%d] >= 1
            } else {
                fail "Key not moved to destination"
            }
            wait_for_ofs_sync [Rn 1] [Rn 4]

            # restart node 4
            if {$with_rdb eq "with"} {
                restart_server -4 true false true save ;# rdb save
                # the asm task info in rdb will fire module event
                assert_equal  [list \
                    "sub: cluster-slot-migration-import-started, source_node_id:$src_id, destination_node_id:$dest_id, task_id:$task_id, slots:0-100" \
                ] [R 4 asm.get_cluster_event_log]
            } else {                
                restart_server -4 true false true nosave ;# no rdb saved
            }
            wait_for_cluster_propagation

            wait_for_asm_done

            # started and completed are paired, and not duplicated
            set import_event_log [list \
                "sub: cluster-slot-migration-import-started, source_node_id:$src_id, destination_node_id:$dest_id, task_id:$task_id, slots:0-100" \
                "sub: cluster-slot-migration-import-completed, source_node_id:$src_id, destination_node_id:$dest_id, task_id:$task_id, slots:0-100" \
            ]
            wait_for_condition 500 10 {
                [R 1 asm.get_cluster_event_log] eq $import_event_log &&
                [R 4 asm.get_cluster_event_log] eq $import_event_log &&
                [R 7 asm.get_cluster_event_log] eq $import_event_log
            } else {
                fail "ASM import event not received"
            }

            R 0 CLUSTER MIGRATION IMPORT 0 100
            wait_for_asm_done
            R 4 save ;# save an empty rdb to override previous one
            clear_module_event_log
            reset_default_trim_method
            R 0 flushall
            R 1 flushall
        }
    }

    test "Test cluster module notifications when replica is disconnected and full resync after importing" {
        clear_module_event_log
        R 1 debug asm-trim-method $trim_method
        R 4 debug asm-trim-method $trim_method
        R 7 debug asm-trim-method $trim_method

        set src_id [R 0 cluster myid]
        set dest_id [R 1 cluster myid]

        # Set a key in the slot range
        set key [slot_key 0 mykey]
        R 0 set $key "value"

        # Start migration, 2s delay
        set task_id [setup_slot_migration_with_delay 0 1 0 100 0 2000000]
        # Wait until at least one key is moved to destination
        wait_for_condition 1000 10 {
            [scan [regexp -inline {keys\=([\d]*)} [R 1 info keyspace]] keys=%d] >= 1
        } else {
            fail "Key not moved to destination"
        }
        wait_for_ofs_sync [Rn 1] [Rn 4]

        # puase node-4
        set r4_pid [S 4 process_id]
        pause_process $r4_pid

        # set a small repl-backlog-size and write some commands to make node-4
        # full resync when reconnecting after waking up
        set r1_full_sync [S 1 sync_full]
        R 1 config set repl-backlog-size 16kb
        R 1 client kill type replica
        set 1k_str [string repeat "a" 1024]
        for {set i 0} {$i < 2000} {incr i} {
            R 1 set [slot_key 6000] $1k_str
        }

        # after ASM task is completed, wake up node-4
        wait_for_condition 1000 10 {
            [CI 1 cluster_slot_migration_active_tasks] == 0 &&
            [CI 1 cluster_slot_migration_active_trim_running] == 0
        } else {
            fail "ASM tasks did not completed"
        }
        resume_process $r4_pid

        # make sure full resync happens
        wait_for_sync [Rn 4]
        wait_for_ofs_sync [Rn 1] [Rn 4]
        assert_morethan [S 1 sync_full] $r1_full_sync

        # started and completed are paired, and not duplicated
        set import_event_log [list \
            "sub: cluster-slot-migration-import-started, source_node_id:$src_id, destination_node_id:$dest_id, task_id:$task_id, slots:0-100" \
            "sub: cluster-slot-migration-import-completed, source_node_id:$src_id, destination_node_id:$dest_id, task_id:$task_id, slots:0-100" \
        ]
        wait_for_condition 500 10 {
            [R 1 asm.get_cluster_event_log] eq $import_event_log &&
            [R 4 asm.get_cluster_event_log] eq $import_event_log &&
            [R 7 asm.get_cluster_event_log] eq $import_event_log
        } else {
            fail "ASM import event not received"
        }

        # since ASM task is completed on node-1 before node-4 reconnects,
        # no trim event should be received on node-4
        assert_equal {} [R 4 asm.get_cluster_trim_event_log]

        R 0 CLUSTER MIGRATION IMPORT 0 100
        wait_for_asm_done
        clear_module_event_log
        reset_default_trim_method
        R 0 flushall
        R 1 flushall
    }

    test "Test new master can trim slots when migration is completed and failover occurs on source side" {
        R 0 asm.disable_trim ;# can not start slot trimming on source side
        set slot0_key [slot_key 0 mykey]
        R 0 set $slot0_key "value"

        # migrate slot 0 from #0 to #1, and wait it completed, but not allow to trim slots
        # on source node
        set task_id [R 1 CLUSTER MIGRATION IMPORT 0 0]
        wait_for_condition 1000 10 {
            [string match {*completed*} [migration_status 0 $task_id state]] &&
            [string match {*completed*} [migration_status 1 $task_id state]]
        } else {
            fail "ASM task did not complete"
        }
        # verify trim is not allowed on source node, and replica node doesn't have trim job either
        wait_for_ofs_sync [Rn 0] [Rn 3]
        assert_equal 1 [R 0 asm.trim_in_progress]
        assert_equal "value" [R 0 asm.read_pending_trim_key $slot0_key]
        assert_equal 0 [R 3 asm.trim_in_progress]
        assert_equal "value" [R 3 asm.read_pending_trim_key $slot0_key]

        set loglines [count_log_lines 0]

        # failover happens on source node, instance #3 become slave, #0 become master
        failover_and_wait_for_done 3
        R 0 asm.enable_trim ;# enable trim on old master

        # old master should cancel the pending trim job
        wait_for_log_messages 0 {"*Cancelling the pending trim job*"} $loglines 1000 10

        wait_for_ofs_sync [Rn 3] [Rn 0]
        # verify trim is allowed on new master, and the key is trimmed
        wait_for_condition 1000 10 {
            [R 3 asm.trim_in_progress] == 0 &&
            [R 3 asm.read_pending_trim_key $slot0_key] eq "" &&
            [R 0 asm.trim_in_progress] == 0 &&
            [R 0 asm.read_pending_trim_key $slot0_key] eq ""
        } else {
            fail "Trim did not complete"
        }

        # verify the trim events, use active trim since module is subscribed to trimmed event
        set trim_event_log [list \
            "sub: cluster-slot-migration-trim-started, slots:0-0" \
            "keyspace: key_trimmed, key: $slot0_key" \
            "sub: cluster-slot-migration-trim-completed, slots:0-0" \
        ]
        wait_for_condition 500 20 {
            [R 0 asm.get_cluster_trim_event_log] eq $trim_event_log &&
            [R 3 asm.get_cluster_trim_event_log] eq $trim_event_log &&
            [R 6 asm.get_cluster_trim_event_log] eq $trim_event_log
        } else {
            fail "ASM destination trim event not received"
        }

        # cleanup
        failover_and_wait_for_done 0
        R 0 CLUSTER MIGRATION IMPORT 0 0
        wait_for_asm_done
        clear_module_event_log
        reset_default_trim_method
        R 0 flushall
        R 1 flushall
    }

    test "Test module replicates commands at the beginning of slot migration " {
        R 0 flushall
        R 1 flushall

        # Sanity check
        assert_equal 0 [R 1 asm.read_keyless_cmd_val]
        assert_equal 0 [R 4 asm.read_keyless_cmd_val]

        # Enable module command replication and set a key to be replicated
        # Module will replicate two commands:
        #  1- A keyless command: asm.keyless_cmd
        #  2- SET command for the given key and value
        set keyname [slot_key 0 modulekey]
        R 0 asm.replicate_module_command 1 $keyname "value"

        setup_slot_migration_with_delay 0 1 0 100
        wait_for_asm_done
        wait_for_ofs_sync [Rn 1] [Rn 4]

        # Verify the commands are replicated
        assert_equal 1 [R 1 asm.read_keyless_cmd_val]
        assert_equal value [R 1 get $keyname]

        # Verify the commands are replicated to replica
        R 4 readonly
        assert_equal 1 [R 4 asm.read_keyless_cmd_val]
        assert_equal value [R 4 get $keyname]

        # cleanup
        R 0 asm.replicate_module_command 0 "" ""
        R 0 CLUSTER MIGRATION IMPORT 0 100
        wait_for_asm_done
        R 0 flushall
        R 1 flushall
    }

    test "Test subcommand propagation during slot migration" {
        R 0 flushall
        R 1 flushall
        set task_id [setup_slot_migration_with_delay 0 1 0 100]

        set key [slot_key 0 mykey]
        R 0 asm.parent set $key "value" ;# execute a module subcommand
        wait_for_asm_done
        assert_equal "value" [R 1 GET $key]

        # cleanup
        R 0 cluster migration import 0 100
        wait_for_asm_done
    }

    test "Test trim method selection based on module keyspace subscription" {
        R 0 debug asm-trim-method default
        R 1 debug asm-trim-method default

        R 0 flushall
        R 1 flushall

        populate_slot 10 -idx 0 -slot 0

        # Make sure module is subscribed to NOTIFY_KEY_TRIMMED event. In this
        # case, active trim must be used.
        R 0 asm.subscribe_trimmed_event 1
        set loglines [count_log_lines 0]
        R 1 CLUSTER MIGRATION IMPORT 0 15
        wait_for_asm_done
        wait_for_log_messages 0 {"*Active trim scheduled for slots: 0-15*"} $loglines 1000 10

        # Move slots back to node-0. Make sure module is not subscribed to
        # NOTIFY_KEY_TRIMMED event. In this case, background trim must be used.
        R 1 asm.subscribe_trimmed_event 0
        set loglines [count_log_lines -1]
        R 0 CLUSTER MIGRATION IMPORT 0 15
        wait_for_asm_done
        wait_for_log_messages -1 {"*Background trim started for slots: 0-15*"} $loglines 1000 10

        # cleanup
        wait_for_asm_done
        R 0 asm.subscribe_trimmed_event 1
        R 1 asm.subscribe_trimmed_event 1
        R 0 flushall
        R 1 flushall
    }

    test "Verify trimmed key value can be read in the server event callback" {
        R 0 flushall
        set key [slot_key 0]
        set value "value123random"
        R 0 set $key $value

        R 1 CLUSTER MIGRATION IMPORT 0 0
        wait_for_asm_done
        wait_for_condition 1000 10 {
            [R 0 asm.get_last_deleted_key] eq "keyevent: key: $key, value: $value"
        } else {
            fail "Last deleted key event not received"
        }

        # cleanup
        R 0 CLUSTER MIGRATION IMPORT 0 0
        wait_for_asm_done
    }

    test "Verify module cannot open a key in a slot that is being trimmed" {
        R 0 flushall
        R 0 debug asm-trim-method active -1 ;# disable active trim

        set key [slot_key 0]
        R 0 set $key value

        R 1 CLUSTER MIGRATION IMPORT 0 0
        wait_for_condition 1000 10 {
            [CI 0 cluster_slot_migration_active_tasks] == 0 &&
            [CI 1 cluster_slot_migration_active_tasks] == 0 &&
            [CI 0 cluster_slot_migration_active_trim_running] == 1
        } else {
            fail "migrate failed"
        }

        # We cannot open the key since it is in a slot being trimmed
        assert_equal {} [R 0 asm.get $key]

        # cleanup
        R 0 debug asm-trim-method default
        R 0 CLUSTER MIGRATION IMPORT 0 0
        wait_for_asm_done
    }

    test "Test RM_ClusterGetLocalSlotRanges" {
       assert_equal [R 0 asm.cluster_get_local_slot_ranges] {{0 5461}}
       assert_equal [R 3 asm.cluster_get_local_slot_ranges] {{0 5461}}

       R 0 cluster migration import 5463 6000
       wait_for_asm_done
       wait_for_cluster_propagation
       assert_equal [R 0 asm.cluster_get_local_slot_ranges] {{0 5461} {5463 6000}}
       assert_equal [R 3 asm.cluster_get_local_slot_ranges] {{0 5461} {5463 6000}}

       R 0 cluster migration import 5462 5462 6001 10922
       wait_for_asm_done
       wait_for_cluster_propagation
       assert_equal [R 0 asm.cluster_get_local_slot_ranges] {{0 10922}}
       assert_equal [R 3 asm.cluster_get_local_slot_ranges] {{0 10922}}
       assert_equal [R 1 asm.cluster_get_local_slot_ranges] {}
       assert_equal [R 4 asm.cluster_get_local_slot_ranges] {}
    }
}

set testmodule [file normalize tests/modules/atomicslotmigration.so]

start_cluster 2 0 [list tags {external:skip cluster modules} config_lines [list loadmodule $testmodule cluster-node-timeout 60000 cluster-allow-replica-migration no appendonly yes]] {
    test "TRIMSLOTS in AOF will work synchronously on restart" {
        # When TRIMSLOTS is replayed from AOF during restart, it must execute
        # synchronously rather than using active trim. This prevents race
        # conditions where subsequent AOF commands might operate on keys
        # that should have been trimmed.

        # Subscribe to key trimmed event to force active trim
        R 0 asm.subscribe_trimmed_event 1
        populate_slot 1000 -slot 0
        populate_slot 1000 -slot 1
        R 1 CLUSTER MIGRATION IMPORT 0 0
        wait_for_asm_done

        # verify active trim is used
        assert_equal 1 [CI 0 cluster_slot_migration_stats_active_trim_completed]

        # restart server and verify aof is loaded
        restart_server 0 yes no yes nosave
        assert {[scan [regexp -inline {aof_current_size:([\d]*)} [R 0 info persistence]] aof_current_size=%d] > 0}
        wait_for_cluster_state "ok"

        # verify TRIMSLOTS in AOF is executed synchronously
        assert_equal 0 [CI 0 cluster_slot_migration_stats_active_trim_completed]
        assert_equal 1000 [R 0 dbsize]

        # cleanup
        R 0 CLUSTER MIGRATION IMPORT 0 0
        wait_for_asm_done
        assert_equal 2000 [R 0 dbsize]
        R 0 flushall
        R 1 flushall
        clear_module_event_log

    }

    test "Test trim is disabled when module requests it" {
        R 0 asm.disable_trim

        set slot0_key [slot_key 0 mykey]
        R 0 set $slot0_key "value"
        set task_id [R 1 CLUSTER MIGRATION IMPORT 0 0]
        wait_for_condition 1000 10 {
            [string match {*completed*} [migration_status 0 $task_id state]]
        } else {
            fail "ASM task did not complete"
        }
        # since we disable trim, the key should still exist on source,
        # we can read it with REDISMODULE_OPEN_KEY_ACCESS_TRIMMED flag
        assert_equal "value" [R 0 asm.read_pending_trim_key $slot0_key]
        assert_equal 1 [R 0 asm.trim_in_progress]

        # enable trim and verify the key is trimmed
        R 0 asm.enable_trim
        wait_for_condition 1000 10 {
            [R 0 asm.read_pending_trim_key $slot0_key] eq "" &&
            [R 0 asm.trim_in_progress] == 0
        } else {
            fail "Trim did not complete"
        }
        wait_for_asm_done
        R 0 CLUSTER MIGRATION IMPORT 0 0
        wait_for_asm_done
        clear_module_event_log
    }

    test "Can not start new asm task when trim is not allowed" {
        # start a migration task, wait it completed but not allow to trim slots
        R 0 asm.disable_trim
        set task_id [R 1 CLUSTER MIGRATION IMPORT 0 0]
        wait_for_condition 1000 10 {
            [string match {*completed*} [migration_status 0 $task_id state]]
        } else {
            fail "ASM task did not complete"
        }
        # Can not start new migrating task since trim is disabled
        set task_id [R 1 CLUSTER MIGRATION IMPORT 1 1]
        wait_for_condition 1000 10 {
            [string match {*fail*} [migration_status 1 $task_id state]] &&
            [string match {*Trim is disabled by module*} [migration_status 1 $task_id last_error]]
        } else {
            fail "ASM task did not fail"
        }
        R 0 asm.enable_trim
        wait_for_asm_done

        # start a migration task, wait it completed but not allow to trim slots
        R 0 asm.disable_trim
        set task_id [R 1 CLUSTER MIGRATION IMPORT 2 2]
        wait_for_condition 1000 10 {
            [string match {*completed*} [migration_status 0 $task_id state]]
        } else {
            fail "ASM task did not complete"
        }
        set logline [count_log_lines 0]
        # Can not start new importing task since trim is disabled
        set task_id [R 0 CLUSTER MIGRATION IMPORT 0 1]
        wait_for_log_messages 0 {"*Can not start import task*trim is disabled by module*"} $logline 1000 10
        R 0 asm.enable_trim
        wait_for_asm_done
    }
}

start_server {tags "cluster external:skip"} {
    test "Test RM_ClusterGetLocalSlotRanges without cluster" {
        r module load $testmodule
        assert_equal [r asm.cluster_get_local_slot_ranges] {{0 16383}}
    }
}
}
