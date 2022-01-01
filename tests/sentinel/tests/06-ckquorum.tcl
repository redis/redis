# Test for the SENTINEL CKQUORUM command

source "../tests/includes/init-tests.tcl"
set num_sentinels [llength $::sentinel_instances]

# According to the SENTINELS MASTER response, get the number of sentinels
# that in the `down` state (s_down or o_down).
proc get_sentinel_sentinels_down_count {sentinel_id master_name} {
    set down_sentinel_count 0
    set sentinels [S $sentinel_id SENTINEL SENTINELS $master_name]

    foreach sentinel $sentinels {
        set flags [dict get $sentinel flags]
        if {[string match *down* $flags]} { incr down_sentinel_count 1 }
    }

    return $down_sentinel_count
}

test "CKQUORUM reports OK and the right amount of Sentinels" {
    foreach_sentinel_id id {
        assert_match "*OK $num_sentinels usable*" [S $id SENTINEL CKQUORUM mymaster]
    }
}

test "CKQUORUM detects quorum cannot be reached" {
    set orig_quorum [expr {$num_sentinels/2+1}]
    S 0 SENTINEL SET mymaster quorum [expr {$num_sentinels+1}]
    catch {[S 0 SENTINEL CKQUORUM mymaster]} err
    assert_match "*NOQUORUM*" $err
    S 0 SENTINEL SET mymaster quorum $orig_quorum
}

test "CKQUORUM detects failover authorization cannot be reached" {
    set orig_quorum [expr {$num_sentinels/2+1}]
    S 0 SENTINEL SET mymaster quorum 1
    for {set i 0} {$i < $orig_quorum} {incr i} {
        kill_instance sentinel [expr {$i + 1}]
    }

    # Wait for the current sentinel to monitor that other sentinels are
    # `down`, so that we can check the quorum directly later.
    wait_for_condition 1000 50 {
        [get_sentinel_sentinels_down_count 0 mymaster] == $orig_quorum
    } else {
        fail "At least $orig_quorum sentinels did not enter the down state."
    }

    assert_error "*NOQUORUM*" {S 0 SENTINEL CKQUORUM mymaster}
    S 0 SENTINEL SET mymaster quorum $orig_quorum
    for {set i 0} {$i < $orig_quorum} {incr i} {
        restart_instance sentinel [expr {$i + 1}]
    }
}

