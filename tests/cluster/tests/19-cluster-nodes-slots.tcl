# Optimize CLUSTER NODES command by generating all nodes slot topology firstly

source "../tests/includes/init-tests.tcl"

test "Create a 2 nodes cluster" {
    cluster_create_with_continuous_slots 2 2
}

test "Cluster should start ok" {
    assert_cluster_state ok
}

set master1 [Rn 0]
set master2 [Rn 1]

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
    assert_match "" [$master1 SFLUSH 2 4]
    assert_match "" [$master1 SFLUSH 0 4]
    assert_match "" [$master2 SFLUSH 0 4]
    assert_match "" [$master1 SFLUSH 1 8191]
    assert_match "" [$master1 SFLUSH 0 8190]
    assert_match "" [$master1 SFLUSH 0 998 2001 8191]
    assert_match "" [$master1 SFLUSH 1 999 2001 8191]
    assert_match "" [$master1 SFLUSH 0 999 2001 8190]
    assert_match "" [$master1 SFLUSH 0 999 2002 8191]
    assert_match "{0 999} {2001 8191}" [$master1 SFLUSH 0 999 2001 8191]
    assert_match "{0 999} {2001 8191}" [$master1 SFLUSH 0 8191]
    assert_match "{0 999} {2001 8191}" [$master1 SFLUSH 0 4000 4001 8191]
    assert_match "" [$master2 SFLUSH 8193 16383]
    assert_match "" [$master2 SFLUSH 8192 16382]
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

test "Continuous slots distribution" {
    assert_match "* 0-8191*" [$master1 CLUSTER NODES]
    assert_match "* 8192-16383*" [$master2 CLUSTER NODES]
    assert_match "*0 8191*" [$master1 CLUSTER SLOTS]
    assert_match "*8192 16383*" [$master2 CLUSTER SLOTS]

    $master1 CLUSTER DELSLOTS 4096
    assert_match "* 0-4095 4097-8191*" [$master1 CLUSTER NODES]
    assert_match "*0 4095*4097 8191*" [$master1 CLUSTER SLOTS]


    $master2 CLUSTER DELSLOTS 12288
    assert_match "* 8192-12287 12289-16383*" [$master2 CLUSTER NODES]
    assert_match "*8192 12287*12289 16383*" [$master2 CLUSTER SLOTS]
}

test "Discontinuous slots distribution" {
    # Remove middle slots
    $master1 CLUSTER DELSLOTS 4092 4094
    assert_match "* 0-4091 4093 4095 4097-8191*" [$master1 CLUSTER NODES]
    assert_match "*0 4091*4093 4093*4095 4095*4097 8191*" [$master1 CLUSTER SLOTS]
    $master2 CLUSTER DELSLOTS 12284 12286
    assert_match "* 8192-12283 12285 12287 12289-16383*" [$master2 CLUSTER NODES]
    assert_match "*8192 12283*12285 12285*12287 12287*12289 16383*" [$master2 CLUSTER SLOTS]

    # Remove head slots
    $master1 CLUSTER DELSLOTS 0 2
    assert_match "* 1 3-4091 4093 4095 4097-8191*" [$master1 CLUSTER NODES]
    assert_match "*1 1*3 4091*4093 4093*4095 4095*4097 8191*" [$master1 CLUSTER SLOTS]

    # Remove tail slots
    $master2 CLUSTER DELSLOTS 16380 16382 16383
    assert_match "* 8192-12283 12285 12287 12289-16379 16381*" [$master2 CLUSTER NODES]
    assert_match "*8192 12283*12285 12285*12287 12287*12289 16379*16381 16381*" [$master2 CLUSTER SLOTS]
}
