# Optimize CLUSTER NODES command by generating all nodes slot topology firstly

start_cluster 2 2 {tags {external:skip cluster}} {

test "Cluster should start ok" {
    wait_for_cluster_state ok
}

set master1 [srv 0 "client"]
set master2 [srv -1 "client"]

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

} ;# start_cluster
