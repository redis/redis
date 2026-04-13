# This test uses a custom slot allocation for testing
proc cluster_allocate_with_continuous_slots_local {n} {
    R 0 cluster ADDSLOTSRANGE 0 3276
    R 1 cluster ADDSLOTSRANGE 3277 6552
    R 2 cluster ADDSLOTSRANGE 6553 9828
    R 3 cluster ADDSLOTSRANGE 9829 13104
    R 4 cluster ADDSLOTSRANGE 13105 16383
}

start_cluster 5 0 {tags {external:skip cluster}} {

set master1 [srv 0 "client"]
set master2 [srv -1 "client"]
set master3 [srv -2 "client"]
set master4 [srv -3 "client"]
set master5 [srv -4 "client"]

test "Continuous slots distribution" {
    assert_match "* 0-3276*" [$master1 CLUSTER NODES]
    assert_match "* 3277-6552*" [$master2 CLUSTER NODES]
    assert_match "* 6553-9828*" [$master3 CLUSTER NODES]
    assert_match "* 9829-13104*" [$master4 CLUSTER NODES]
    assert_match "* 13105-16383*" [$master5 CLUSTER NODES]
    assert_match "*0 3276*" [$master1 CLUSTER SLOTS]
    assert_match "*3277 6552*" [$master2 CLUSTER SLOTS]
    assert_match "*6553 9828*" [$master3 CLUSTER SLOTS]
    assert_match "*9829 13104*" [$master4 CLUSTER SLOTS]
    assert_match "*13105 16383*" [$master5 CLUSTER SLOTS]

    $master1 CLUSTER DELSLOTSRANGE 3001 3050
    assert_match "* 0-3000 3051-3276*" [$master1 CLUSTER NODES]
    assert_match "*0 3000*3051 3276*" [$master1 CLUSTER SLOTS]

    $master2 CLUSTER DELSLOTSRANGE 5001 5500
    assert_match "* 3277-5000 5501-6552*" [$master2 CLUSTER NODES]
    assert_match "*3277 5000*5501 6552*" [$master2 CLUSTER SLOTS]

    $master3 CLUSTER DELSLOTSRANGE 7001 7100 8001 8500
    assert_match "* 6553-7000 7101-8000 8501-9828*" [$master3 CLUSTER NODES]
    assert_match "*6553 7000*7101 8000*8501 9828*" [$master3 CLUSTER SLOTS]

    $master4 CLUSTER DELSLOTSRANGE 11001 12000 12101 12200
    assert_match "* 9829-11000 12001-12100 12201-13104*" [$master4 CLUSTER NODES]
    assert_match "*9829 11000*12001 12100*12201 13104*" [$master4 CLUSTER SLOTS]

    $master5 CLUSTER DELSLOTSRANGE 13501 14000 15001 16000
    assert_match "* 13105-13500 14001-15000 16001-16383*" [$master5 CLUSTER NODES]
    assert_match "*13105 13500*14001 15000*16001 16383*" [$master5 CLUSTER SLOTS]
}

test "ADDSLOTS command with several boundary conditions test suite" {
    assert_error "ERR Invalid or out of range slot" {R 0 cluster ADDSLOTS 3001 aaa}
    assert_error "ERR Invalid or out of range slot" {R 0 cluster ADDSLOTS 3001 -1000}
    assert_error "ERR Invalid or out of range slot" {R 0 cluster ADDSLOTS 3001 30003}
    
    assert_error "ERR Slot 3200 is already busy" {R 0 cluster ADDSLOTS 3200}
    assert_error "ERR Slot 8501 is already busy" {R 0 cluster ADDSLOTS 8501}

    assert_error "ERR Slot 3001 specified multiple times" {R 0 cluster ADDSLOTS 3001 3002 3001}
}

test "ADDSLOTSRANGE command with several boundary conditions test suite" {
    # Add multiple slots with incorrect argument number
    assert_error "ERR wrong number of arguments for 'cluster|addslotsrange' command" {R 0 cluster ADDSLOTSRANGE 3001 3020 3030}

    # Add multiple slots with invalid input slot
    assert_error "ERR Invalid or out of range slot" {R 0 cluster ADDSLOTSRANGE 3001 3020 3030 aaa}
    assert_error "ERR Invalid or out of range slot" {R 0 cluster ADDSLOTSRANGE 3001 3020 3030 70000}
    assert_error "ERR Invalid or out of range slot" {R 0 cluster ADDSLOTSRANGE 3001 3020 -1000 3030}

    # Add multiple slots when start slot number is greater than the end slot
    assert_error "ERR start slot number 3030 is greater than end slot number 3025" {R 0 cluster ADDSLOTSRANGE 3001 3020 3030 3025}

    # Add multiple slots with busy slot
    assert_error "ERR Slot 3200 is already busy" {R 0 cluster ADDSLOTSRANGE 3001 3020 3200 3250}

    # Add multiple slots with assigned multiple times
    assert_error "ERR Slot 3001 specified multiple times" {R 0 cluster ADDSLOTSRANGE 3001 3020 3001 3020}
}

test "DELSLOTSRANGE command with several boundary conditions test suite" {
    # Delete multiple slots with incorrect argument number
    assert_error "ERR wrong number of arguments for 'cluster|delslotsrange' command" {R 0 cluster DELSLOTSRANGE 1000 2000 2100}
    assert_match "* 0-3000 3051-3276*" [$master1 CLUSTER NODES]
    assert_match "*0 3000*3051 3276*" [$master1 CLUSTER SLOTS]

    # Delete multiple slots with invalid input slot
    assert_error "ERR Invalid or out of range slot" {R 0 cluster DELSLOTSRANGE 1000 2000 2100 aaa}
    assert_error "ERR Invalid or out of range slot" {R 0 cluster DELSLOTSRANGE 1000 2000 2100 70000}
    assert_error "ERR Invalid or out of range slot" {R 0 cluster DELSLOTSRANGE 1000 2000 -2100 2200}
    assert_match "* 0-3000 3051-3276*" [$master1 CLUSTER NODES]
    assert_match "*0 3000*3051 3276*" [$master1 CLUSTER SLOTS]

    # Delete multiple slots when start slot number is greater than the end slot
    assert_error "ERR start slot number 5800 is greater than end slot number 5750" {R 1 cluster DELSLOTSRANGE 5600 5700 5800 5750}
    assert_match "* 3277-5000 5501-6552*" [$master2 CLUSTER NODES]
    assert_match "*3277 5000*5501 6552*" [$master2 CLUSTER SLOTS]

    # Delete multiple slots with already unassigned
    assert_error "ERR Slot 7001 is already unassigned" {R 2 cluster DELSLOTSRANGE 7001 7100 9000 9200}
    assert_match "* 6553-7000 7101-8000 8501-9828*" [$master3 CLUSTER NODES]
    assert_match "*6553 7000*7101 8000*8501 9828*" [$master3 CLUSTER SLOTS]

    # Delete multiple slots with assigned multiple times
    assert_error "ERR Slot 12500 specified multiple times" {R 3 cluster DELSLOTSRANGE 12500 12600 12500 12600}
    assert_match "* 9829-11000 12001-12100 12201-13104*" [$master4 CLUSTER NODES]
    assert_match "*9829 11000*12001 12100*12201 13104*" [$master4 CLUSTER SLOTS]
}
} cluster_allocate_with_continuous_slots_local

start_cluster 2 0 {tags {external:skip cluster experimental}} {

set master1 [srv 0 "client"]
set master2 [srv -1 "client"]

test "SFLUSH - Errors and output validation" {
    assert_match "* 0-8191*" [$master1 CLUSTER NODES]
    assert_match "* 8192-16383*" [$master2 CLUSTER NODES]
    assert_match "*0 8191*" [$master1 CLUSTER SLOTS]
    assert_match "*8192 16383*" [$master2 CLUSTER SLOTS]

    # make master1 non-continuous slots
    $master1 cluster DELSLOTSRANGE 1000 2000

    # Test SFLUSH errors validation
    assert_error {ERR wrong number of arguments*}           {$master1 SFLUSH 4}
    assert_error {ERR wrong number of arguments*}           {$master1 SFLUSH 4 SYNC}
    assert_error {ERR Invalid or out of range slot}         {$master1 SFLUSH x 4}
    assert_error {ERR Invalid or out of range slot}         {$master1 SFLUSH 0 12x}
    assert_error {ERR Slot 3 specified multiple times}      {$master1 SFLUSH 2 4 3 5}
    assert_error {ERR start slot number 8 is greater than*} {$master1 SFLUSH 8 4}
    assert_error {ERR wrong number of arguments*}           {$master1 SFLUSH 4 8 10}
    assert_error {ERR wrong number of arguments*}           {$master1 SFLUSH 0 999 2001 8191 ASYNCX}

    # Test SFLUSH output validation
    assert_match "{2 4}" [$master1 SFLUSH 2 4]
    assert_match "{0 4}" [$master1 SFLUSH 0 4]
    assert_match "" [$master2 SFLUSH 0 4]
    assert_match "{1 999} {2001 8191}" [$master1 SFLUSH 1 8191]
    assert_match "{0 999} {2001 8190}" [$master1 SFLUSH 0 8190]
    assert_match "{0 998} {2001 8191}" [$master1 SFLUSH 0 998 2001 8191]
    assert_match "{1 999} {2001 8191}" [$master1 SFLUSH 1 999 2001 8191]
    assert_match "{0 999} {2001 8190}" [$master1 SFLUSH 0 999 2001 8190]
    assert_match "{0 999} {2002 8191}" [$master1 SFLUSH 0 999 2002 8191]
    assert_match "{0 999} {2001 8191}" [$master1 SFLUSH 0 999 2001 8191]
    assert_match "{0 999} {2001 8191}" [$master1 SFLUSH 0 8191]
    assert_match "{0 999} {2001 8191}" [$master1 SFLUSH 0 4000 4001 8191]
    assert_match "{8193 16383}" [$master2 SFLUSH 8193 16383]
    assert_match "{8192 16382}" [$master2 SFLUSH 8192 16382]
    assert_match "{8192 16383}" [$master2 SFLUSH 8192 16383]
    assert_match "{8192 16383}" [$master2 SFLUSH 8192 16383 SYNC]
    assert_match "{8192 16383}" [$master2 SFLUSH 8192 16383 ASYNC]
    assert_match "{8192 16383}" [$master2 SFLUSH 8192 9000 9001 16383]
    assert_match "{8192 16383}" [$master2 SFLUSH 8192 9000 9001 16383 SYNC]
    assert_match "{8192 16383}" [$master2 SFLUSH 8192 9000 9001 16383 ASYNC]

    # restore master1 continuous slots
    $master1 cluster ADDSLOTSRANGE 1000 2000
}

test "SFLUSH - Deletes the keys with argument <NONE>/SYNC/ASYNC" {
    foreach op {"" "SYNC" "ASYNC"} {
        for {set i 0} {$i < 100} {incr i} {
            catch {$master1 SET key$i val$i}
            catch {$master2 SET key$i val$i}
        }

        assert {[$master1 DBSIZE] > 0}
        assert {[$master2 DBSIZE] > 0}
        if {$op eq ""} {
            assert_match "{0 8191}" [ $master1 SFLUSH 0 8191]
        } else {
            assert_match "{0 8191}" [ $master1 SFLUSH 0 8191 $op]
        }
        assert {[$master1 DBSIZE] == 0}
        assert {[$master2 DBSIZE] > 0}
        assert_match "{8192 16383}" [ $master2 SFLUSH 8192 16383]
        assert {[$master2 DBSIZE] == 0}
    }
}

}

set testmodule [file normalize tests/modules/atomicslotmigration.so]
start_cluster 2 2 [list tags {external:skip cluster experimental modules} config_lines [list loadmodule $testmodule]] {
foreach sync_method {"SYNC" "BLOCKING-ASYNC" "ASYNC"} {
foreach trim_method {"active" "bg"} {
    test "sflush can propagate to replicas (sync method: $sync_method, trim method: $trim_method)" {
        R 0 flushall
        R 0 debug asm-trim-method $trim_method
        R 2 debug asm-trim-method $trim_method

        # Add keys in master
        R 0 set "06S" "slot0"
        wait_for_ofs_sync [Rn 0] [Rn 2]

        set loglines [count_log_lines 0]
        set loglines2 [count_log_lines -2]

        # since we have optimization, if the master is not running in blocking context,
        # we will try to run in blocking ASYNC mode, so we need to use MULTI/EXEC to make it blocking
        if {$sync_method eq "SYNC"} {
            R 0 MULTI
        }

        # Execute SFLUSH on master, SYNC will be run as blocking ASYNC if not running in MULTI/EXEC
        set sync_option "SYNC"
        if {$sync_method eq "ASYNC"} {
            set sync_option "ASYNC"
        }
        R 0 SFLUSH 0 8190 $sync_option

        # Execute EXEC if using SYNC
        if {$sync_method eq "SYNC"} {
            R 0 EXEC
        }

        # Wait for SFLUSH to propagate to replica, and complete the trim
        wait_for_condition 1000 10 {
           [R 0 DBSIZE] == 0 && [R 2 DBSIZE] == 0
        } else {
            fail "SFLUSH did not propagate to replica"
        }

        if {$sync_method ne "SYNC"} {
            if {$trim_method eq "active"} {
                wait_for_log_messages 0 {"*Active trim completed for slots*0-8190*"} $loglines 1000 10
                wait_for_log_messages -2 {"*Active trim completed for slots*0-8190*"} $loglines2 1000 10
            } else {
                # background trim
                wait_for_log_messages 0 {"*Background trim started for slots*0-8190*"} $loglines 1000 10
                wait_for_log_messages -2 {"*Background trim started for slots*0-8190*"} $loglines2 1000 10
            }
        }
    }
}
}
    test "Canceling active trimming can unblock sflush" {
        # Delay active trim to make sure it is not completed before FLUSHDB
        R 0 debug asm-trim-method active 10000 ;# delay 10ms per key
        # Add slot 0 keys in master
        for {set i 0} {$i < 1000} {incr i} {
            R 0 set "{06S}$i" "value$i"
        }

        set rd [redis_deferring_client 0]
        $rd SFLUSH 0 8190 SYNC ;# running in blocking async method

        # FLUSHDB will cancel all trim jobs
        R 0 SELECT 0
        R 0 FLUSHDB SYNC

        # SFLUSH should be unblocked and return empty array
        assert_equal [$rd read] "{0 8190}"
        $rd close
    }

    test "Write is rejected and read is allowed in SFLUSH slots using active trim" {
        R 0 debug asm-trim-method active 1000 ;# delay 1ms per key
        R 0 asm.clear_event_log
        R 2 asm.clear_event_log

        # Add slot 0 keys
        for {set i 0} {$i < 1000} {incr i} {
            R 0 set "{06S}$i" "value$i"
        }
        # Add a slot 1 key, we should trim slot 0 first, then slot 1
        set slot1_key "Qi"
        R 0 set $slot1_key "slot1"
        wait_for_ofs_sync [Rn 0] [Rn 2]

        set rd [redis_deferring_client 0]
        $rd SFLUSH 0 8190 SYNC ;# running in blocking async method

        # we can read the slot 1 key
        assert_equal [R 0 get $slot1_key] "slot1"
        # Module with flag REDISMODULE_OPEN_KEY_ACCESS_TRIMMED also can read the key
        assert_equal [R 0 asm.read_pending_trim_key $slot1_key] "slot1"

        # we can not write to the slot 1 key
        assert_error "*TRYAGAIN Slot is being trimmed*" {R 0 set $slot1_key "value1"}

        # wait for SFLUSH to complete
        assert_equal [$rd read] "{0 8190}"
        $rd close

        # there is no trim event since we sfluh the owned slots of this node
        assert_equal [R 0 asm.get_cluster_event_log] {}
        assert_equal [R 2 asm.get_cluster_event_log] {}
    }

    test "SFLUSH all local slots uses flushdb optimization (no trim)" {
        R 0 flushall
        R 0 debug asm-trim-method active

        # Add keys in slot 0
        for {set i 0} {$i < 100} {incr i} {
            R 0 set "{06S}$i" "value$i"
        }
        assert {[R 0 DBSIZE] == 100}
        wait_for_ofs_sync [Rn 0] [Rn 2]
        assert {[R 2 DBSIZE] == 100}

        set prev_trim_done [CI 0 cluster_slot_migration_stats_active_trim_completed]
        set prev_trim_done2 [CI 2 cluster_slot_migration_stats_active_trim_completed]

        # SFLUSH with multiple ranges that together cover all local slots.
        # If the selected slots are exactly the same as the local slots, we can
        # simply flush the entire DB.
        assert_match "{0 8191}" [R 0 SFLUSH 0 1000 1001 5000 5001 8191]
        assert {[R 0 DBSIZE] == 0}

        # Verify replica is also flushed
        wait_for_ofs_sync [Rn 0] [Rn 2]
        assert {[R 2 DBSIZE] == 0}

        # Verify active_trim_completed counter did NOT increase since it will trigger
        # flush (similar to flushdb command) instead of triggering trim.
        assert_equal [CI 0 cluster_slot_migration_stats_active_trim_completed] $prev_trim_done
        assert_equal [CI 2 cluster_slot_migration_stats_active_trim_completed] $prev_trim_done2
    }
}
