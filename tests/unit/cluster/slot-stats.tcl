#
# Copyright (c) 2009-Present, Redis Ltd.
# All rights reserved.
#
# Copyright (c) 2024-present, Valkey contributors.
# All rights reserved.
#
# Licensed under your choice of (a) the Redis Source Available License 2.0
# (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
# GNU Affero General Public License v3 (AGPLv3).
#
# Portions of this file are available under BSD3 terms; see REDISCONTRIBUTIONS for more information.
#

# Integration tests for CLUSTER SLOT-STATS command.

# -----------------------------------------------------------------------------
# Helper functions for CLUSTER SLOT-STATS test cases.
# -----------------------------------------------------------------------------

# Converts array RESP response into a dict.
# This is useful for many test cases, where unnecessary nesting is removed.
proc convert_array_into_dict {slot_stats} {
    set res [dict create]
    foreach slot_stat $slot_stats {
        # slot_stat is an array of size 2, where 0th index represents (int) slot,
        # and 1st index represents (map) usage statistics.
        dict set res [lindex $slot_stat 0] [lindex $slot_stat 1]
    }
    return $res
}

proc get_cmdstat_usec {cmd r} {
    set cmdstatline [cmdrstat $cmd r]
    regexp "usec=(.*?),usec_per_call=(.*?),rejected_calls=0,failed_calls=0" $cmdstatline -> usec _
    return $usec
}

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

proc assert_empty_slot_stats {slot_stats metrics_to_assert} {
    set slot_stats [convert_array_into_dict $slot_stats]
    dict for {slot stats} $slot_stats {
        foreach metric_name $metrics_to_assert {
            set metric_value [dict get $stats $metric_name]
            assert {$metric_value == 0}
        }
    }
}

proc assert_empty_slot_stats_with_exception {slot_stats exception_slots metrics_to_assert} {
    set slot_stats [convert_array_into_dict $slot_stats]
    dict for {slot stats} $exception_slots {
        assert {[dict exists $slot_stats $slot]} ;# slot_stats must contain the expected slots.
    }
    dict for {slot stats} $slot_stats {
        if {[dict exists $exception_slots $slot]} {
            foreach metric_name $metrics_to_assert {
                set metric_value [dict get $exception_slots $slot $metric_name]
                assert {[dict get $stats $metric_name] == $metric_value}
            }
        } else {
            dict for {metric value} $stats {
                assert {$value == 0}
            }
        }
    }
}

proc assert_equal_slot_stats {slot_stats_1 slot_stats_2 deterministic_metrics non_deterministic_metrics} {
    set slot_stats_1 [convert_array_into_dict $slot_stats_1]
    set slot_stats_2 [convert_array_into_dict $slot_stats_2]
    assert {[dict size $slot_stats_1] == [dict size $slot_stats_2]}

    dict for {slot stats_1} $slot_stats_1 {
        assert {[dict exists $slot_stats_2 $slot]}
        set stats_2 [dict get $slot_stats_2 $slot]

        # For deterministic metrics, we assert their equality.
        foreach metric $deterministic_metrics {
            assert {[dict get $stats_1 $metric] == [dict get $stats_2 $metric]}
        }
        # For non-deterministic metrics, we assert their non-zeroness as a best-effort.
        foreach metric $non_deterministic_metrics {
            assert {([dict get $stats_1 $metric] == 0 && [dict get $stats_2 $metric] == 0) || \
                    ([dict get $stats_1 $metric] != 0 && [dict get $stats_2 $metric] != 0)}
        }
    }
}

proc assert_all_slots_have_been_seen {expected_slots} {
    dict for {k v} $expected_slots {
        assert {$v == 1}
    }
}

proc assert_slot_visibility {slot_stats expected_slots} {
    set slot_stats [convert_array_into_dict $slot_stats]
    dict for {slot _} $slot_stats {
        assert {[dict exists $expected_slots $slot]}
        dict set expected_slots $slot 1
    }

    assert_all_slots_have_been_seen $expected_slots
}

proc assert_slot_stats_monotonic_order {slot_stats orderby is_desc} {
    # For Tcl dict, the order of iteration is the order in which the keys were inserted into the dictionary
    # Thus, the response ordering is preserved upon calling 'convert_array_into_dict()'.
    # Source: https://www.tcl.tk/man/tcl8.6.11/TclCmd/dict.htm
    set slot_stats [convert_array_into_dict $slot_stats]
    set prev_metric -1
    dict for {_ stats} $slot_stats {
        set curr_metric [dict get $stats $orderby]
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
# Test cases for CLUSTER SLOT-STATS cpu-usec metric correctness.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster} overrides {cluster-slot-stats-enabled yes}} {

    # Define shared variables.
    set key "FOO"
    set key_slot [R 0 cluster keyslot $key]
    set key_secondary "FOO2"
    set key_secondary_slot [R 0 cluster keyslot $key_secondary]
    set metrics_to_assert [list cpu-usec]

    test "CLUSTER SLOT-STATS cpu-usec reset upon CONFIG RESETSTAT." {
        R 0 SET $key VALUE
        R 0 DEL $key
        R 0 CONFIG RESETSTAT
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS cpu-usec reset upon slot migration." {
        R 0 SET $key VALUE

        R 0 CLUSTER DELSLOTS $key_slot
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert

        R 0 CLUSTER ADDSLOTS $key_slot
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS cpu-usec for non-slot specific commands." {
        R 0 INFO
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS cpu-usec for slot specific commands." {
        R 0 SET $key VALUE
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set usec [get_cmdstat_usec set r]
        set expected_slot_stats [
            dict create $key_slot [
                dict create cpu-usec $usec
            ]
        ]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS cpu-usec for blocking commands, unblocked on keyspace update." {
        # Blocking command with no timeout. Only keyspace update can unblock this client.
        set rd [redis_deferring_client]
        $rd BLPOP $key 0
        wait_for_blocked_clients_count 1
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        # When the client is blocked, no accumulation is made. This behaviour is identical to INFO COMMANDSTATS.
        assert_empty_slot_stats $slot_stats $metrics_to_assert

        # Unblocking command.
        R 0 LPUSH $key value
        wait_for_blocked_clients_count 0

        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set lpush_usec [get_cmdstat_usec lpush r]
        set blpop_usec [get_cmdstat_usec blpop r]

        # Assert that both blocking and non-blocking command times have been accumulated.
        set expected_slot_stats [
            dict create $key_slot [
                dict create cpu-usec [expr $lpush_usec + $blpop_usec]
            ]
        ]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS cpu-usec for blocking commands, unblocked on timeout." {
        # Blocking command with 0.5 seconds timeout.
        set rd [redis_deferring_client]
        $rd BLPOP $key 0.5

        # Confirm that the client is blocked, then unblocked within 1 second.
        wait_for_blocked_clients_count 1
        wait_for_blocked_clients_count 0

        # Assert that the blocking command time has been accumulated.
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set blpop_usec [get_cmdstat_usec blpop r]
        set expected_slot_stats [
            dict create $key_slot [
                dict create cpu-usec $blpop_usec
            ]
        ]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS cpu-usec for transactions." {
        set r1 [redis_client]
        $r1 MULTI
        $r1 SET $key value
        $r1 GET $key

        # CPU metric is not accumulated until EXEC is reached. This behaviour is identical to INFO COMMANDSTATS.
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert

        # Execute transaction, and assert that all nested command times have been accumulated.
        $r1 EXEC
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set exec_usec [get_cmdstat_usec exec r]
        set expected_slot_stats [
            dict create $key_slot [
                dict create cpu-usec $exec_usec
            ]
        ]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS cpu-usec for lua-scripts, without cross-slot keys." {
        r eval [format "#!lua
            redis.call('set', '%s', 'bar'); redis.call('get', '%s')" $key $key] 0

        set eval_usec [get_cmdstat_usec eval r]
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]

        set expected_slot_stats [
            dict create $key_slot [
                dict create cpu-usec $eval_usec
            ]
        ]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS cpu-usec for lua-scripts, with cross-slot keys." {
        r eval [format "#!lua flags=allow-cross-slot-keys
            redis.call('set', '%s', 'bar'); redis.call('get', '%s');
        " $key $key_secondary] 0

        # For cross-slot, we do not accumulate at all.
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS cpu-usec for functions, without cross-slot keys." {
        set function_str [format "#!lua name=f1
            redis.register_function{
                function_name='f1',
                callback=function() redis.call('set', '%s', '1') redis.call('get', '%s') end
            }" $key $key]
        r function load replace $function_str
        r fcall f1 0

        set fcall_usec [get_cmdstat_usec fcall r]
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]

        set expected_slot_stats [
            dict create $key_slot [
                dict create cpu-usec $fcall_usec
            ]
        ]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS cpu-usec for functions, with cross-slot keys." {
        set function_str [format "#!lua name=f1
            redis.register_function{
                function_name='f1',
                callback=function() redis.call('set', '%s', '1') redis.call('get', '%s') end,
                flags={'allow-cross-slot-keys'}
            }" $key $key_secondary]
        r function load replace $function_str
        r fcall f1 0

        # For cross-slot, we do not accumulate at all.
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL
}

# -----------------------------------------------------------------------------
# Test cases for CLUSTER SLOT-STATS network-bytes-in.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster} overrides {cluster-slot-stats-enabled yes}} {

    # Define shared variables.
    set key "key"
    set key_slot [R 0 cluster keyslot $key]
    set metrics_to_assert [list network-bytes-in]

    test "CLUSTER SLOT-STATS network-bytes-in, multi bulk buffer processing." {
        # *3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n --> 33 bytes.
        R 0 SET $key value

        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set expected_slot_stats [
            dict create $key_slot [
                dict create network-bytes-in 33
            ]
        ]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS network-bytes-in, in-line buffer processing." {
        set rd [redis_deferring_client]
        # SET key value\r\n --> 15 bytes.
        $rd write "SET $key value\r\n"
        $rd flush

        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set expected_slot_stats [
            dict create $key_slot [
                dict create network-bytes-in 15
            ]
        ]

        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS network-bytes-in, blocking command." {
        set rd [redis_deferring_client]
        # *3\r\n$5\r\nblpop\r\n$3\r\nkey\r\n$1\r\n0\r\n --> 31 bytes.
        $rd BLPOP $key 0
        wait_for_blocked_clients_count 1

        # Slot-stats must be empty here, as the client is yet to be unblocked.
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert

        # *3\r\n$5\r\nlpush\r\n$3\r\nkey\r\n$5\r\nvalue\r\n --> 35 bytes.
        R 0 LPUSH $key value
        wait_for_blocked_clients_count 0

        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set expected_slot_stats [
            dict create $key_slot [
                dict create network-bytes-in 66 ;# 31 + 35 bytes.
            ]
        ]

        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS network-bytes-in, multi-exec transaction." {
        set r [redis_client]
        # *1\r\n$5\r\nmulti\r\n --> 15 bytes.
        $r MULTI
        # *3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n --> 33 bytes.
        assert {[$r SET $key value] eq {QUEUED}}
        # *1\r\n$4\r\nexec\r\n --> 14 bytes.
        assert {[$r EXEC] eq {OK}}

        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set expected_slot_stats [
            dict create $key_slot [
                dict create network-bytes-in 62 ;# 15 + 33 + 14 bytes.
            ]
        ]

        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS network-bytes-in, non slot specific command." {
        R 0 INFO

        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS network-bytes-in, pub/sub." {
        # PUB/SUB does not get accumulated at per-slot basis,
        # as it is cluster-wide and is not slot specific.
        set rd [redis_deferring_client]
        $rd subscribe channel
        R 0 publish channel message

        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL
}

start_cluster 1 1 {tags {external:skip cluster} overrides {cluster-slot-stats-enabled yes}} {
    set channel "channel"
    set key_slot [R 0 cluster keyslot $channel]
    set metrics_to_assert [list network-bytes-in]

    # Setup replication.
    assert {[s -1 role] eq {slave}}
    wait_for_condition 1000 50 {
        [s -1 master_link_status] eq {up}
    } else {
        fail "Instance #1 master link status is not up"
    }
    R 1 readonly

    test "CLUSTER SLOT-STATS network-bytes-in, sharded pub/sub." {
        set slot [R 0 cluster keyslot $channel]
        set primary [Rn 0]
        set replica [Rn 1]
        set replica_subcriber [redis_deferring_client -1]
        $replica_subcriber SSUBSCRIBE $channel
        # *2\r\n$10\r\nssubscribe\r\n$7\r\nchannel\r\n --> 34 bytes.
        $primary SPUBLISH $channel hello
        # *3\r\n$8\r\nspublish\r\n$7\r\nchannel\r\n$5\r\nhello\r\n --> 42 bytes.

        set slot_stats [$primary CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set expected_slot_stats [
            dict create $key_slot [
                dict create network-bytes-in 42
            ]
        ]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert

        set slot_stats [$replica CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set expected_slot_stats [
            dict create $key_slot [
                dict create network-bytes-in 34
            ]
        ]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL
}

# -----------------------------------------------------------------------------
# Test cases for CLUSTER SLOT-STATS network-bytes-out correctness.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster}} {
    # Define shared variables.
    set key "FOO"
    set key_slot [R 0 cluster keyslot $key]
    set expected_slots_to_key_count [dict create $key_slot 1]
    set metrics_to_assert [list network-bytes-out]
    R 0 CONFIG SET cluster-slot-stats-enabled yes

    test "CLUSTER SLOT-STATS network-bytes-out, for non-slot specific commands." {
        R 0 INFO
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS network-bytes-out, for slot specific commands." {
        R 0 SET $key value
        # +OK\r\n --> 5 bytes

        set expected_slot_stats [
            dict create $key_slot [
                dict create network-bytes-out 5
            ]
        ]
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL

    test "CLUSTER SLOT-STATS network-bytes-out, blocking commands." {
        set rd [redis_deferring_client]
        $rd BLPOP $key 0
        wait_for_blocked_clients_count 1

        # Assert empty slot stats here, since COB is yet to be flushed due to the block.
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert

        # Unblock the command.
        # LPUSH client) :1\r\n --> 4 bytes.
        # BLPOP client) *2\r\n$3\r\nkey\r\n$5\r\nvalue\r\n --> 24 bytes, upon unblocking.
        R 0 LPUSH $key value
        wait_for_blocked_clients_count 0

        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set expected_slot_stats [
            dict create $key_slot [
                dict create network-bytes-out 28 ;# 4 + 24 bytes.
            ]
        ]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    R 0 CONFIG RESETSTAT
    R 0 FLUSHALL
}

start_cluster 1 1 {tags {external:skip cluster}} {

    # Define shared variables.
    set key "FOO"
    set key_slot [R 0 CLUSTER KEYSLOT $key]
    set metrics_to_assert [list network-bytes-out]
    R 0 CONFIG SET cluster-slot-stats-enabled yes

    # Setup replication.
    assert {[s -1 role] eq {slave}}
    wait_for_condition 1000 50 {
        [s -1 master_link_status] eq {up}
    } else {
        fail "Instance #1 master link status is not up"
    }
    R 1 readonly

    test "CLUSTER SLOT-STATS network-bytes-out, replication stream egress." {
        assert_equal [R 0 SET $key VALUE] {OK}
        # Local client) +OK\r\n --> 5 bytes.
        # Replication stream) *3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n --> 33 bytes.
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set expected_slot_stats [
            dict create $key_slot [
                dict create network-bytes-out 38 ;# 5 + 33 bytes.
            ]
        ]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
}

start_cluster 1 1 {tags {external:skip cluster}} {

    # Define shared variables.
    set channel "channel"
    set key_slot [R 0 cluster keyslot $channel]
    set channel_secondary "channel2"
    set key_slot_secondary [R 0 cluster keyslot $channel_secondary]
    set metrics_to_assert [list network-bytes-out]
    R 0 CONFIG SET cluster-slot-stats-enabled yes

    test "CLUSTER SLOT-STATS network-bytes-out, sharded pub/sub, single channel." {
        set slot [R 0 cluster keyslot $channel]
        set publisher [Rn 0]
        set subscriber [redis_client]
        set replica [redis_deferring_client -1]

        # Subscriber client) *3\r\n$10\r\nssubscribe\r\n$7\r\nchannel\r\n:1\r\n --> 38 bytes
        $subscriber SSUBSCRIBE $channel
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set expected_slot_stats [
            dict create $key_slot [
                dict create network-bytes-out 38
            ]
        ]
        R 0 CONFIG RESETSTAT

        # Publisher client) :1\r\n --> 4 bytes.
        # Subscriber client) *3\r\n$8\r\nsmessage\r\n$7\r\nchannel\r\n$5\r\nhello\r\n --> 42 bytes.
        assert_equal 1 [$publisher SPUBLISH $channel hello]
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set expected_slot_stats [
            dict create $key_slot [
                dict create network-bytes-out 46 ;# 4 + 42 bytes.
            ]
        ]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
    $subscriber QUIT
    R 0 FLUSHALL
    R 0 CONFIG RESETSTAT

    test "CLUSTER SLOT-STATS network-bytes-out, sharded pub/sub, cross-slot channels." {
        set slot [R 0 cluster keyslot $channel]
        set publisher [Rn 0]
        set subscriber [redis_client]
        set replica [redis_deferring_client -1]

        # Stack multi-slot subscriptions against a single client.
        # For primary channel;
        # Subscriber client) *3\r\n$10\r\nssubscribe\r\n$7\r\nchannel\r\n:1\r\n --> 38 bytes
        # For secondary channel;
        # Subscriber client) *3\r\n$10\r\nssubscribe\r\n$8\r\nchannel2\r\n:1\r\n --> 39 bytes
        $subscriber SSUBSCRIBE $channel
        $subscriber SSUBSCRIBE $channel_secondary
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set expected_slot_stats [
            dict create \
                $key_slot [ \
                    dict create network-bytes-out 38
                ] \
                $key_slot_secondary [ \
                    dict create network-bytes-out 39
                ]
        ]
        R 0 CONFIG RESETSTAT

        # For primary channel;
        # Publisher client) :1\r\n --> 4 bytes.
        # Subscriber client) *3\r\n$8\r\nsmessage\r\n$7\r\nchannel\r\n$5\r\nhello\r\n --> 42 bytes.
        # For secondary channel;
        # Publisher client) :1\r\n --> 4 bytes.
        # Subscriber client) *3\r\n$8\r\nsmessage\r\n$8\r\nchannel2\r\n$5\r\nhello\r\n --> 43 bytes.
        assert_equal 1 [$publisher SPUBLISH $channel hello]
        assert_equal 1 [$publisher SPUBLISH $channel_secondary hello]
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        set expected_slot_stats [
            dict create \
                $key_slot [ \
                    dict create network-bytes-out 46 ;# 4 + 42 bytes.
                ] \
                $key_slot_secondary [ \
                    dict create network-bytes-out 47 ;# 4 + 43 bytes.
                ]
        ]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }
}

# -----------------------------------------------------------------------------
# Test cases for CLUSTER SLOT-STATS key-count metric correctness.
# -----------------------------------------------------------------------------

start_cluster 1 0 {tags {external:skip cluster} overrides {cluster-slot-stats-enabled yes}} {

    # Define shared variables.
    set key "FOO"
    set key_slot [R 0 cluster keyslot $key]
    set metrics_to_assert [list key-count]
    set expected_slot_stats [
        dict create $key_slot [
            dict create key-count 1
        ]
    ]

    test "CLUSTER SLOT-STATS contains default value upon redis-server startup" {
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert
    }

    test "CLUSTER SLOT-STATS contains correct metrics upon key introduction" {
        R 0 SET $key TEST
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }

    test "CLUSTER SLOT-STATS contains correct metrics upon key mutation" {
        R 0 SET $key NEW_VALUE
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats_with_exception $slot_stats $expected_slot_stats $metrics_to_assert
    }

    test "CLUSTER SLOT-STATS contains correct metrics upon key deletion" {
        R 0 DEL $key
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats $slot_stats $metrics_to_assert
    }

    test "CLUSTER SLOT-STATS slot visibility based on slot ownership changes" {
        R 0 CONFIG SET cluster-require-full-coverage no

        R 0 CLUSTER DELSLOTS $key_slot
        set expected_slots [initialize_expected_slots_dict]
        dict unset expected_slots $key_slot
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert {[dict size $expected_slots] == 16383}
        assert_slot_visibility $slot_stats $expected_slots

        R 0 CLUSTER ADDSLOTS $key_slot
        set expected_slots [initialize_expected_slots_dict]
        set slot_stats [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
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

start_cluster 1 0 {tags {external:skip cluster} overrides {cluster-slot-stats-enabled yes}} {

    set metrics [list "key-count" "cpu-usec" "network-bytes-in" "network-bytes-out"]

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
        foreach orderby $metrics {
            set slot_stats [R 0 CLUSTER SLOT-STATS ORDERBY $orderby DESC]
            assert_slot_stats_monotonic_descent $slot_stats $orderby
        }
    }

    test "CLUSTER SLOT-STATS ORDERBY ASC correct ordering" {
        foreach orderby $metrics {
            set slot_stats [R 0 CLUSTER SLOT-STATS ORDERBY $orderby ASC]
            assert_slot_stats_monotonic_ascent $slot_stats $orderby
        }
    }

    test "CLUSTER SLOT-STATS ORDERBY LIMIT correct response pagination, where limit is less than number of assigned slots" {
        R 0 FLUSHALL SYNC
        R 0 CONFIG RESETSTAT

        foreach orderby $metrics {
            set limit 5
            set slot_stats_desc [R 0 CLUSTER SLOT-STATS ORDERBY $orderby LIMIT $limit DESC]
            set slot_stats_asc [R 0 CLUSTER SLOT-STATS ORDERBY $orderby LIMIT $limit ASC]
            set slot_stats_desc_length [llength $slot_stats_desc]
            set slot_stats_asc_length [llength $slot_stats_asc]
            assert {$limit == $slot_stats_desc_length && $limit == $slot_stats_asc_length}

            # All slot statistics have been reset to 0, so we will order by slot in ascending order.
            set expected_slots [dict create 0 0 1 0 2 0 3 0 4 0]
            assert_slot_visibility $slot_stats_desc $expected_slots
            assert_slot_visibility $slot_stats_asc $expected_slots
        }
    }

    test "CLUSTER SLOT-STATS ORDERBY LIMIT correct response pagination, where limit is greater than number of assigned slots" {
        R 0 CONFIG SET cluster-require-full-coverage no
        R 0 FLUSHALL SYNC
        R 0 CLUSTER FLUSHSLOTS
        R 0 CLUSTER ADDSLOTS 100 101

        foreach orderby $metrics {
            set num_assigned_slots 2
            set limit 5
            set slot_stats_desc [R 0 CLUSTER SLOT-STATS ORDERBY $orderby LIMIT $limit DESC]
            set slot_stats_asc [R 0 CLUSTER SLOT-STATS ORDERBY $orderby LIMIT $limit ASC]
            set slot_stats_desc_length [llength $slot_stats_desc]
            set slot_stats_asc_length [llength $slot_stats_asc]
            set expected_response_length [expr min($num_assigned_slots, $limit)]
            assert {$expected_response_length == $slot_stats_desc_length && $expected_response_length == $slot_stats_asc_length}

            set expected_slots [dict create 100 0 101 0]
            assert_slot_visibility $slot_stats_desc $expected_slots
            assert_slot_visibility $slot_stats_asc $expected_slots
        }
    }

    test "CLUSTER SLOT-STATS ORDERBY arg sanity check." {
        # Non-existent argument.
        assert_error "ERR*" {R 0 CLUSTER SLOT-STATS ORDERBY key-count non-existent-arg}
        # Negative LIMIT.
        assert_error "ERR*" {R 0 CLUSTER SLOT-STATS ORDERBY key-count DESC LIMIT -1}
        # Non-existent ORDERBY metric.
        assert_error "ERR*" {R 0 CLUSTER SLOT-STATS ORDERBY non-existent-metric}
        # When cluster-slot-stats-enabled config is disabled, you cannot sort using advanced metrics.
        R 0 CONFIG SET cluster-slot-stats-enabled no
        set orderby "cpu-usec"
        assert_error "ERR*" {R 0 CLUSTER SLOT-STATS ORDERBY $orderby}
        set orderby "network-bytes-in"
        assert_error "ERR*" {R 0 CLUSTER SLOT-STATS ORDERBY $orderby}
        set orderby "network-bytes-out"
        assert_error "ERR*" {R 0 CLUSTER SLOT-STATS ORDERBY $orderby}
    }

}

# -----------------------------------------------------------------------------
# Test cases for CLUSTER SLOT-STATS replication.
# -----------------------------------------------------------------------------

start_cluster 1 1 {tags {external:skip cluster} overrides {cluster-slot-stats-enabled yes}} {

    # Define shared variables.
    set key "key"
    set key_slot [R 0 CLUSTER KEYSLOT $key]
    set primary [Rn 0]
    set replica [Rn 1]

    # For replication, assertions are split between deterministic and non-deterministic metrics.
    # * For deterministic metrics, strict equality assertions are made.
    # * For non-deterministic metrics, non-zeroness assertions are made.
    #   Non-zeroness as in, both primary and replica should either have some value, or no value at all.
    #
    # * key-count is deterministic between primary and its replica.
    # * cpu-usec is non-deterministic between primary and its replica.
    # * network-bytes-in is deterministic between primary and its replica.
    # * network-bytes-out will remain empty in the replica, since primary client do not receive replies, unless for replicationSendAck().
    set deterministic_metrics [list key-count network-bytes-in]
    set non_deterministic_metrics [list cpu-usec]
    set empty_metrics [list network-bytes-out]

    # Setup replication.
    assert {[s -1 role] eq {slave}}
    wait_for_condition 1000 50 {
        [s -1 master_link_status] eq {up}
    } else {
        fail "Instance #1 master link status is not up"
    }
    R 1 readonly

    test "CLUSTER SLOT-STATS metrics replication for new keys" {
        # *3\r\n$3\r\nset\r\n$3\r\nkey\r\n$5\r\nvalue\r\n --> 33 bytes.
        R 0 SET $key VALUE

        set expected_slot_stats [
            dict create $key_slot [
                dict create key-count 1 network-bytes-in 33
            ]
        ]
        set slot_stats_master [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats_with_exception $slot_stats_master $expected_slot_stats $deterministic_metrics

        wait_for_condition 500 10 {
            [string match {*calls=1,*} [cmdrstat set $replica]]
        } else {
            fail "Replica did not receive the command."
        }
        set slot_stats_replica [R 1 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_equal_slot_stats $slot_stats_master $slot_stats_replica $deterministic_metrics $non_deterministic_metrics
        assert_empty_slot_stats $slot_stats_replica $empty_metrics
    }
    R 0 CONFIG RESETSTAT
    R 1 CONFIG RESETSTAT

    test "CLUSTER SLOT-STATS metrics replication for existing keys" {
        # *3\r\n$3\r\nset\r\n$3\r\nkey\r\n$13\r\nvalue_updated\r\n --> 42 bytes.
        R 0 SET $key VALUE_UPDATED

        set expected_slot_stats [
            dict create $key_slot [
                dict create key-count 1 network-bytes-in 42
            ]
        ]
        set slot_stats_master [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats_with_exception $slot_stats_master $expected_slot_stats $deterministic_metrics

        wait_for_condition 500 10 {
            [string match {*calls=1,*} [cmdrstat set $replica]]
        } else {
            fail "Replica did not receive the command."
        }
        set slot_stats_replica [R 1 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_equal_slot_stats $slot_stats_master $slot_stats_replica $deterministic_metrics $non_deterministic_metrics
        assert_empty_slot_stats $slot_stats_replica $empty_metrics
    }
    R 0 CONFIG RESETSTAT
    R 1 CONFIG RESETSTAT

    test "CLUSTER SLOT-STATS metrics replication for deleting keys" {
        # *2\r\n$3\r\ndel\r\n$3\r\nkey\r\n --> 22 bytes.
        R 0 DEL $key

        set expected_slot_stats [
            dict create $key_slot [
                dict create key-count 0 network-bytes-in 22
            ]
        ]
        set slot_stats_master [R 0 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_empty_slot_stats_with_exception $slot_stats_master $expected_slot_stats $deterministic_metrics

        wait_for_condition 500 10 {
            [string match {*calls=1,*} [cmdrstat del $replica]]
        } else {
            fail "Replica did not receive the command."
        }
        set slot_stats_replica [R 1 CLUSTER SLOT-STATS SLOTSRANGE 0 16383]
        assert_equal_slot_stats $slot_stats_master $slot_stats_replica $deterministic_metrics $non_deterministic_metrics
        assert_empty_slot_stats $slot_stats_replica $empty_metrics
    }
    R 0 CONFIG RESETSTAT
    R 1 CONFIG RESETSTAT
}
