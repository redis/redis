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
    6000 "{4L7}" \
    6001 "{4YV}" \
    6002 "{0bx}" \
    6003 "{AJ}" \
    6004 "{of}" \
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

# Wait for all ASM tasks to complete in the cluster
proc wait_for_asm_done {} {
    set total_instances [expr {$::cluster_master_nodes + $::cluster_replica_nodes}]

    for {set i 0} {$i < $total_instances} {incr i} {
        wait_for_condition 1000 10 {
            [CI $i cluster_slot_migration_task_count] == 0
        } else {
            set migration_count [CI $i cluster_slot_migration_task_count]
            fail "ASM tasks did not complete on instance $i: migration_tasks=$migration_count"
        }
    }
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

# Setup slot migration test with keys and 2s delay, then start migration
# Returns the task_id for the migration
proc setup_slot_migration_with_delay {src_node dst_node start_slot end_slot} {
    # Two keys on the start slot
    set key1 [slot_key $start_slot key1]
    set key2 [slot_key $start_slot key2]
    R $src_node set $key1 "a"
    R $src_node set $key2 "b"

    # we set a delay to ensure migration takes time for testing,
    # two keys cost 2s to save
    R $src_node config set rdb-key-save-delay 1000000

    # migrate slot range from src_node to dst_node
    set task_id [R $dst_node CLUSTER MIGRATION IMPORT $start_slot $end_slot]
    wait_for_condition 2000 10 {
        [string match {*send-bulk-and-stream*} [migration_status $src_node $task_id state]]
    } else {
        fail "ASM task did not start"
    }

    return $task_id
}

start_cluster 3 3 {tags {external:skip cluster} overrides {cluster-node-timeout 30000 cluster-allow-replica-migration no}} {
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

        # revert the migration
        R 1 CLUSTER MIGRATION IMPORT 7000 8000
        wait_for_asm_done
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
            set load_handle0 [start_write_load "127.0.0.1" $port 100 $key]
        }

        # Start write traffic on node-1
        # Throws -MOVED error once asm is completed, catch block will ignore it.
        catch {
            # Start the slot 6000 write load on the R 1
            set port [get_port 1]
            set key [slot_key 6000 mykey]
            set load_handle1 [start_write_load "127.0.0.1" $port 100 $key]
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
        assert_equal [R 0 dbsize] [R 1 dbsize]
        assert_equal [R 0 debug digest] [R 1 debug digest]

        # cleanup
        R 0 debug asm-trim-method default
        R 0 flushall
        R 1 debug asm-trim-method default
        R 1 flushall

        R 0 CLUSTER MIGRATION IMPORT 0 100
        wait_for_asm_done
        R 1 CLUSTER MIGRATION IMPORT 6000 6100
        wait_for_asm_done
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
        wait_for_asm_done

        # the appended 99 times should also be migrated
        assert_equal [string repeat a 100] [R 1 get $slot0_key]
        assert_equal [string repeat b 100] [R 1 get $slot1_key]
        # the slave should also get the data
        wait_for_ofs_sync [Rn 1] [Rn 4]
        R 4 readonly
        assert_equal [string repeat a 100] [R 4 get $slot0_key]
        assert_equal [string repeat b 100] [R 4 get $slot1_key]

        # verify key that was not in the slot range is not migrated
        assert_equal [string repeat c 100] [R 0 get $slot101_key]
        # verify changes in replica
        wait_for_ofs_sync [Rn 0] [Rn 3]
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
            set load_handle [start_write_load "127.0.0.1" [get_port 1] 500 "06S"]

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
        set slot0_key "06S"
        set slot1_key "Qi"
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
        assert_not_equal [string repeat a 100] [R 0 get $slot0_key]
        assert_equal [string repeat b 100] [R 0 get $slot1_key]
        # Slave should also get the data
        after 100
        R 3 readonly
        assert_equal [string repeat b 100] [R 3 get $slot1_key]
        R 1 config set rdb-key-save-delay 0
    }

    test "Client output buffer limit is reached on source side" {
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
        set load_handle [start_write_load "127.0.0.1" [get_port 0] 1000 $slot0_key]

        # After some time, the client output buffer limit should be reached
        wait_for_log_messages 0 {"*Client * closed * for overcoming of output buffer limits.*"} $loglines 1000 10
        assert_match {*send-bulk-and-stream*} [migration_status 0 $task_id last_error]

        stop_write_load $load_handle

        # resume server and clear pause point
        resume_process $r1_pid
        R 1 debug repl-pause clear

        # Wait for the migration to complete
        wait_for_asm_done

        # Reset configurations
        R 0 config set client-output-buffer-limit "replica 0 0 0"
        R 0 config set rdb-key-save-delay 0
    }

    test "Expired key is not deleted and SCAN/KEYS/RANDOMKEY hide keys in importing slots" {
        set slot0_key "{06S}X"
        set slot1_key "Qi"
        set slot2_key "5L5"
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
        R 1 expire $slot0_key 100
        R 1 expire $slot1_key 80
        R 1 expire $slot2_key 60
        R 1 hincrbyfloat $slot2_key "f1" 1
        R 1 hexpire $slot2_key 60 FIELDS 1 "f1"

        # after 2s, at least a key should be transferred, and should not be deleted
        # due to expired, neither active nor lazy expiration (SCAN) takes effect,
        # Besides SCAN/KEYS/RANDOMKEY command can not find them
        after 2000
        foreach id {0 3} { ;# 0 is the master, 3 is the replica
            assert_equal {0 {}} [R $id scan 0 count 10]
            assert_equal {} [R $id keys "*"]
            assert_equal {} [R $id keys "{06S}*"]
            assert_equal {} [R $id randomkey]
            if {$::verbose} { puts [R $id info keyspace] }
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

            # KEYS/SCAN/RANDOMKEY will find the keys after migration
            assert_equal [list 0 [list $slot0_key $slot1_key $slot2_key]] [R $id scan 0 count 10]
            assert_equal [list $slot0_key $slot1_key $slot2_key] [R $id keys "*"]
            assert_equal [list $slot0_key] [R $id keys "{06S}*"]
            assert_not_equal {} [R $id randomkey]

            # INFO KEYSPACE will also reflect the keys
            assert_equal 3 [scan [regexp -inline {keys\=([\d]*)} [R $id info keyspace]] keys=%d]
            assert_equal 3 [scan [regexp -inline {expires\=([\d]*)} [R $id info keyspace]] expires=%d]
            assert_equal 1 [scan [regexp -inline {subexpiry\=([\d]*)} [R $id info keyspace]] subexpiry=%d]
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
        set slot0_key "06S"
        set slot1_key "Qi"
        set slot2_key "5L5"
        set slot5462_key "450"
        set slot5463_key "4dY"
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
        set r1_mem_used [getInfoProperty [R 1 info memory] used_memory]
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
        if {$::verbose} { puts [R 1 info keyspace] }
        assert_equal 0 [R 1 exists $slot5462_key]
        assert_equal 0 [R 1 exists $slot5463_key]
        assert {[scan [regexp -inline {keys\=([\d]*)} [R 1 info keyspace]] keys=%d] >= 2}

        # current used memory should be more than the maxmemory, since the big keys that
        # belong importing slots can not be evicted.
        set r1_mem_used  [getInfoProperty [R 1 info memory] used_memory]
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
        R 3 cluster failover
        wait_for_condition 1000 50 {
            [getInfoProperty [R 3 info] role] eq {master}
        } else {
            fail "Instance #3 is not a master after some time"
        }

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
        R 0 cluster failover
        wait_for_condition 1000 50 {
            [getInfoProperty [R 0 info] role] eq {master}
        } else {
            fail "Instance #0 is not a master after some time"
        }

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
        # we set a delay to cancel
        R 1 config set rdb-key-save-delay 1000000

        # flushall, flushdb, sflush
        foreach flushcmd {flushall flushdb sflush} {
            # start slot migration from 1 to 0
            set task_id [setup_slot_migration_with_delay 1 0 0 100]

            if {$::verbose} { puts "flush command: $flushcmd"}
            if {$flushcmd == "flushall"} {
                R 0 flushall
            } elseif {$flushcmd == "flushdb"} {
                R 0 flushdb
            } elseif {$flushcmd == "sflush"} {
                R 1 sflush 10 110
            }

            # flush-like will cancel the task
            wait_for_condition 1000 50 {
                [string match {*canceled*} [migration_status 0 $task_id state]] ||
                [string match {*canceled*} [migration_status 1 $task_id state]]
            } else {
                fail "ASM task did not cancel"
            }
        }

        # Since sflush is executed on the source, the task is only canceled on the source.
        # The destination node will retry the import task, and eventually the slot 0-100
        # migration to #0 will succeed.
        R 1 config set rdb-key-save-delay 0
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
        wait_for_condition 2000 50 {
            [string match {*done*} [migration_status 0 $task_id state]] &&
            [string match {*done*} [migration_status 1 $task_id state]]
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
}
