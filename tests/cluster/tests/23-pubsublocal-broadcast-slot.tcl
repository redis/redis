
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

    set primary [Rn 0]
    set replica [Rn 1]

    $replica deferred 1
    $replica SUBSCRIBELOCAL ch1
    $replica read

    set localdata "inGodWeTrust"
    $primary PUBLISHLOCAL ch1 $localdata

    set msg [$replica read]
    assert {$localdata eq [lindex $msg 2]}

    $replica deferred 0
    $replica UNSUBSCRIBELOCAL ch1
}