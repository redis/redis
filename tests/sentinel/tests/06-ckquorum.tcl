# Test for the SENTINEL CKQUORUM command

source "../tests/includes/init-tests.tcl"
set num_sentinels [llength $::sentinel_instances]

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

proc get_sentinel_chquorum_result {sentinel_id master_name} {
    catch {[S $sentinel_id SENTINEL CKQUORUM $master_name]} result
    return $result
}

test "CKQUORUM detects failover authorization cannot be reached" {
    set orig_quorum [expr {$num_sentinels/2+1}]
    S 0 SENTINEL SET mymaster quorum 1
    for {set i 0} {$i < $orig_quorum} {incr i} {
        kill_instance sentinel [expr {$i + 1}]
    }

    # Make sure other sentinels are `down`.
    wait_for_condition 300 50 {
        [string match "*NOQUORUM*" [get_sentinel_chquorum_result 0 mymaster]]
    } else {
        fail "At least $orig_quorum sentinels did not enter the down state."
    }

    S 0 SENTINEL SET mymaster quorum $orig_quorum
    for {set i 0} {$i < $orig_quorum} {incr i} {
        restart_instance sentinel [expr {$i + 1}]
    }
}

