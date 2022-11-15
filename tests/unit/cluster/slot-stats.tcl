# -----------------------------------------------------------------------------
# Helper functions for CLUSTER SLOT-STATS test cases.
# -----------------------------------------------------------------------------

proc initialize_expected_slots_dict {} {
    set expected_slots [dict create]
    for {set i 0} {$i < 16384} {incr i 1} {
        dict set expected_slots $i 0
    }
    return $expected_slots
}

proc initialize_expected_slots_dict_with_range {start_slot end_slot} {
    assert {$start_slot <= $end_slot}
    set expected_slots [dict create]
    for {set i $start_slot} {$i <= $end_slot} {incr i 1} {
        dict set expected_slots $i 0
    }
    return $expected_slots
}

proc get_slot_stats_slot {slot_stats slot_index} {
    assert {[expr {$slot_index%2}] == 0}
    return [lindex $slot_stats $slot_index 0]
}

proc get_slot_stats_key_count {slot_stats slot_index} {
    assert {[expr {$slot_index%2}] == 0}
    return [lindex $slot_stats [expr {$slot_index+1}] 1]
}

proc get_slot_stats_given_orderby_column {slot_stats orderby index} {
    if {$orderby == "key_count"} {
        return [get_slot_stats_key_count $slot_stats $index]
    }
    # TODO: Add "cpu_usage" metric support. See issue #11423.
    fail "Given orderby column $orderby is not supported."
}

proc assert_empty_slot_stats {slot_stats} {
    set slot_stats_length [llength $slot_stats]

    for {set i 0} {$i < $slot_stats_length} {incr i 2} {
        set slot [get_slot_stats_slot $slot_stats $i]
        assert {[get_slot_stats_key_count $slot_stats $i] == 0}
    }
}

proc assert_empty_slot_stats_with_exception {slot_stats exception_slots} {
    set slot_stats_length [llength $slot_stats]

    for {set i 0} {$i < $slot_stats_length} {incr i 2} {
        set slot [get_slot_stats_slot $slot_stats $i]

        if {[dict exists $exception_slots $slot]} {
            set expected_key_count [dict get $exception_slots $slot]
            assert {[get_slot_stats_key_count $slot_stats $i] == $expected_key_count}
        } else {
            assert {[get_slot_stats_key_count $slot_stats $i] == 0}
        }
    }
}

proc assert_equal_slot_stats {slot_stats_1 slot_stats_2} {
    set slot_stats_length [llength $slot_stats_1]
    assert {$slot_stats_length == [llength $slot_stats_2]}
    
    for {set i 0} {$i < $slot_stats_length} {incr i 2} {
        assert {[lindex $slot_stats_1 $i 0] == [lindex $slot_stats_2 $i 0]}
        set sub_list_1 [lindex $slot_stats_1 [expr {$i+1}]]
        set sub_list_2 [lindex $slot_stats_2 [expr {$i+1}]]
        
        set sub_list_length [llength $sub_list_1]
        assert {$sub_list_length == [llength $sub_list_2]}

        for {set j 0} {$j < $sub_list_length} {incr j 1} {
            assert {[lindex $sub_list_1 $j] == [lindex $sub_list_2 $j]}
        }
    }
}

proc assert_all_slots_have_been_seen {expected_slots} {
    dict for {k v} $expected_slots {
        assert {$v == 1}
    }
}

proc assert_slot_visibility {slot_stats expected_slots} {
    set slot_stats_length [llength $slot_stats]
    assert {$slot_stats_length/2 == [dict size $expected_slots]}

    for {set i 0} {$i < $slot_stats_length} {incr i 2} {
        set slot [get_slot_stats_slot $slot_stats $i]
        assert {[dict exists $expected_slots $slot]}
        dict set expected_slots $slot 1
    }

    assert_all_slots_have_been_seen $expected_slots
}

proc assert_slot_stats_key_count {slot_stats expected_slots_key_count} {
    set slot_stats_length [llength $slot_stats]

    for {set i 0} {$i < $slot_stats_length} {incr i 2} {
        set slot [get_slot_stats_slot $slot_stats $i]

        if {[dict exists $expected_slots_key_count $slot]} {
            set key_count [get_slot_stats_key_count $slot_stats $i]
            set key_count_expected [get_slot_stats_key_count $slot_stats $i]
            assert {$key_count == $key_count_expected}
        }
    }
}

proc assert_slot_stats_monotonic_order {slot_stats orderby is_desc} {
    set slot_stats_length [llength $slot_stats]
    set prev_metric -1
    for {set i 0} {$i < $slot_stats_length} {incr i 2} {
        set curr_metric [get_slot_stats_given_orderby_column $slot_stats $orderby $i]
        if {$prev_metric != -1} {
            if {$is_desc == 1} {
                assert {$prev_metric >= $curr_metric}
            } else {
                assert {$prev_metric <= $curr_metric}
            }
        }
        set prev_metric $curr_metric
    }
}

proc assert_slot_stats_monotonic_descent {slot_stats orderby} {
    assert_slot_stats_monotonic_order $slot_stats $orderby 1
}

proc assert_slot_stats_monotonic_ascent {slot_stats orderby} {
    assert_slot_stats_monotonic_order $slot_stats $orderby 0
}

proc wait_for_replica_key_exists {key key_count} {
    wait_for_condition 1000 50 {
        [R 1 exists $key] eq "$key_count"
    } else {
        fail "Test key was not replicated"
    }
}

# -----------------------------------------------------------------------------
# Test cases for CLUSTER SLOT-STATS correctness, without additional arguments.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster}} {

    # Define shared variables.
    set key "FOO"
    set key_slot [R 0 cluster keyslot $key]
    set expected_slots_to_key_count [dict create $key_slot 1]

    test "CLUSTER SLOT-STATS contains default value upon redis-server startup" {
        set slot_stats [R 0 CLUSTER SLOT-STATS]
        assert_empty_slot_stats $slot_stats
    }

    test "CLUSTER SLOT-STATS contains correct metrics upon key introduction" {
        R 0 SET $key TEST
        set slot_stats [R 0 CLUSTER SLOT-STATS]
        set slot_stats_length [lindex $slot_stats]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slots_to_key_count
    }

    test "CLUSTER SLOT-STATS contains correct metrics upon key mutation" {
        R 0 SET $key NEW_VALUE
        set slot_stats [R 0 CLUSTER SLOT-STATS]
        set slot_stats_length [lindex $slot_stats]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slots_to_key_count
    }

    test "CLUSTER SLOT-STATS contains correct metrics upon key deletion" {
        set key "FOO"
        R 0 DEL $key
        set slot_stats [R 0 CLUSTER SLOT-STATS]
        assert_empty_slot_stats $slot_stats
    }

    test "CLUSTER SLOT-STATS slot visibility based on slot ownership changes" {
        R 0 CONFIG SET cluster-require-full-coverage no
        
        R 0 CLUSTER DELSLOTS $key_slot
        set expected_slots [initialize_expected_slots_dict]
        dict unset expected_slots $key_slot
        set slot_stats [R 0 CLUSTER SLOT-STATS]
        assert {[dict size $expected_slots] == 16383}
        assert_slot_visibility $slot_stats $expected_slots

        R 0 CLUSTER ADDSLOTS $key_slot
        set expected_slots [initialize_expected_slots_dict]
        set slot_stats [R 0 CLUSTER SLOT-STATS]
        assert {[dict size $expected_slots] == 16384}
        assert_slot_visibility $slot_stats $expected_slots
    }
}

# -----------------------------------------------------------------------------
# Test cases for CLUSTER SLOT-STATS SLOTSRANGE sub-argument.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster}} {

    test "CLUSTER SLOT-STATS SLOTSRANGE all slots present" {
        set start_slot 100
        set end_slot 102
        set expected_slots [initialize_expected_slots_dict_with_range $start_slot $end_slot]

        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE $start_slot $end_slot]
        assert_slot_visibility $slot_stats $expected_slots
    }

    test "CLUSTER SLOT-STATS SLOTSRANGE some slots missing" {
        set start_slot 100
        set end_slot 102
        set expected_slots [initialize_expected_slots_dict_with_range $start_slot $end_slot]

        R 0 CLUSTER DELSLOTS $start_slot
        dict unset expected_slots $start_slot

        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE $start_slot $end_slot]
        assert_slot_visibility $slot_stats $expected_slots
    }
}

# -----------------------------------------------------------------------------
# Test cases for CLUSTER SLOT-STATS ORDERBY sub-argument.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster}} {
    
    # SET keys for target hashslots, to encourage ordering.
    set hash_tags [list 0 1 2 3 4]
    set num_keys 1
    foreach hash_tag $hash_tags {
        for {set i 0} {$i < $num_keys} {incr i 1} {
            R 0 SET "$i{$hash_tag}" VALUE
        }
        incr num_keys 1
    }

    # SET keys for random hashslots, for random noise.
    set num_keys 0
    while {$num_keys < 1000} {
        set random_key [randomInt 16384]
        R 0 SET $random_key VALUE
        incr num_keys 1
    }

    test "CLUSTER SLOT-STATS ORDERBY DESC correct ordering" {
        set orderby "key_count"
        set slot_stats [R 0 CLUSTER SLOT-STATS ORDERBY $orderby DESC]
        assert_slot_stats_monotonic_descent $slot_stats $orderby
    }

    test "CLUSTER SLOT-STATS ORDERBY ASC correct ordering" {
        set orderby "key_count"
        set slot_stats [R 0 CLUSTER SLOT-STATS ORDERBY $orderby ASC]
        assert_slot_stats_monotonic_ascent $slot_stats $orderby
    }

    test "CLUSTER SLOT-STATS ORDERBY LIMIT correct response pagination, where limit is less than number of assigned slots" {
        set limit 5
        set slot_stats_desc [R 0 CLUSTER SLOT-STATS ORDERBY key_count LIMIT $limit DESC]
        set slot_stats_asc [R 0 CLUSTER SLOT-STATS ORDERBY key_count LIMIT $limit ASC]
        set slot_stats_desc_length [expr {[llength $slot_stats_desc]/2}]
        set slot_stats_asc_length [expr {[llength $slot_stats_asc]/2}]
        assert {$limit == $slot_stats_desc_length && $limit == $slot_stats_asc_length}
    }

    test "CLUSTER SLOT-STATS ORDERBY LIMIT correct response pagination, where limit is greater than number of assigned slots" {
        R 0 CONFIG SET cluster-require-full-coverage no
        R 0 FLUSHALL SYNC
        R 0 CLUSTER FLUSHSLOTS
        R 0 CLUSTER ADDSLOTS 100 101

        set num_assigned_slots 2
        set limit 5
        set slot_stats_desc [R 0 CLUSTER SLOT-STATS ORDERBY key_count LIMIT $limit DESC]
        set slot_stats_asc [R 0 CLUSTER SLOT-STATS ORDERBY key_count LIMIT $limit ASC]
        set slot_stats_desc_length [expr {[llength $slot_stats_desc]/2}]
        set slot_stats_asc_length [expr {[llength $slot_stats_asc]/2}]
        set expected_response_length [expr min($num_assigned_slots, $limit)]
        assert {$expected_response_length == $slot_stats_desc_length && $expected_response_length == $slot_stats_asc_length}
    }
}

# -----------------------------------------------------------------------------
# Test cases for CLUSTER SLOT-STATS replication.
# -----------------------------------------------------------------------------

start_cluster 1 1 {tags {external:skip cluster}} {
    
    # Define shared variables.
    set key "FOO"
    set key_slot [R 0 CLUSTER KEYSLOT $key]

    # Setup replication.
    assert {[s -1 role] eq {slave}}
    wait_for_condition 1000 50 {
        [s -1 master_link_status] eq {up}
    } else {
        fail "Instance #1 master link status is not up"
    }
    R 1 readonly

    test "CLUSTER SLOT-STATS key_count replication for new keys" {
        R 0 SET $key VALUE
        set slot_stats_master [R 0 CLUSTER SLOT-STATS]

        set expected_slots_key_count [dict create $key_slot 1]
        assert_slot_stats_key_count $slot_stats_master $expected_slots_key_count
        wait_for_replica_key_exists $key 1

        set slot_stats_replica [R 1 CLUSTER SLOT-STATS]
        assert_equal_slot_stats $slot_stats_master $slot_stats_replica
    }

    test "CLUSTER SLOT-STATS key_count replication for existing keys" {
        R 0 SET $key VALUE_UPDATED
        set slot_stats_master [R 0 CLUSTER SLOT-STATS]

        set expected_slots_key_count [dict create $key_slot 1]
        assert_slot_stats_key_count $slot_stats_master $expected_slots_key_count
        wait_for_replica_key_exists $key 1

        set slot_stats_replica [R 1 CLUSTER SLOT-STATS]
        assert_equal_slot_stats $slot_stats_master $slot_stats_replica
    }

    test "CLUSTER SLOT-STATS key_count replication for deleting keys" {
        R 0 DEL $key
        set slot_stats_master [R 0 CLUSTER SLOT-STATS]

        set expected_slots_key_count [dict create $key_slot 0]
        assert_slot_stats_key_count $slot_stats_master $expected_slots_key_count
        wait_for_replica_key_exists $key 0

        set slot_stats_replica [R 1 CLUSTER SLOT-STATS]
        assert_equal_slot_stats $slot_stats_master $slot_stats_replica
    }
}
