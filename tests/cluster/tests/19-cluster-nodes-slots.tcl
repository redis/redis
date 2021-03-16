# Optimize CLUSTER NODES command by generating all nodes slot topology firstly

source "../tests/includes/init-tests.tcl"

proc cluster_allocate_with_continuous_slots {n} {
    set slot 16383
    set avg [expr ($slot+1) / $n]
    while {$slot >= 0} {
        set node [expr $slot/$avg >= $n ? $n-1 : $slot/$avg]
        lappend slots_$node $slot
        incr slot -1
    }
    for {set j 0} {$j < $n} {incr j} {
        R $j cluster addslots {*}[set slots_${j}]
    }
}

proc cluster_create_with_continuous_slots {masters slaves} {
    cluster_allocate_with_continuous_slots $masters
    if {$slaves} {
        cluster_allocate_slaves $masters $slaves
    }
    assert_cluster_state ok
}

test "Create a 2 nodes cluster" {
    cluster_create_with_continuous_slots 2 2
}

test "Cluster should start ok" {
    assert_cluster_state ok
}

set master1 [Rn 0]
set master2 [Rn 1]

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
