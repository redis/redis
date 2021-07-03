
# Check data broadcast across master/replica for a slot.

source "../tests/includes/init-tests.tcl"

test "Create a primary with a replica" {
    create_cluster 1 1
}

test "Cluster should start ok" {
    assert_cluster_state ok
}

set primary [Rn 0]
set replica [Rn 1]

test "Subscribe to replica, publish from primary" {

    $replica deferred 1
    $replica SUBSCRIBELOCAL ch1
    $replica read

    set localdata "inGodWeTrust"
    $primary PUBLISHLOCAL ch1 $localdata

    set msg [$replica read]
    assert {$localdata eq [lindex $msg 2]}

    $primary close
    $replica close
}

test "Subscribe to primary, publish from replica" {

    $primary deferred 1
    $primary SUBSCRIBELOCAL ch1
    $primary read

    set localdata "inGodWeTrust"
    $replica PUBLISHLOCAL ch1 $localdata

    set msg [$primary read]
    assert {$localdata eq [lindex $msg 2]}

    $primary close
    $replica close
}