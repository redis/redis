# Check the basic monitoring and failover capabilities.

source "../tests/includes/init-tests.tcl"

if {$::simulate_error} {
    test "This test will fail" {
        fail "Simulated error"
    }
}

test "Different nodes have different IDs" {
    set ids {}
    set numnodes 0
    foreach_redis_id id {
        incr numnodes
        # Every node should just know itself.
        set nodeid [dict get [get_myself $id] id]
        assert {$nodeid ne {}}
        lappend ids $nodeid
    }
    set numids [llength [lsort -unique $ids]]
    assert {$numids == $numnodes}
}

test "It is possible to perform slot allocation" {
    cluster_allocate_slots 5
}

test "After the join, every node gets a different config epoch" {
    set trynum 60
    while {[incr trynum -1] != 0} {
        # We check that this condition is true for *all* the nodes.
        set ok 1 ; # Will be set to 0 every time a node is not ok.
        foreach_redis_id id {
            set epochs {}
            foreach n [get_cluster_nodes $id] {
                lappend epochs [dict get $n config_epoch]
            }
            if {[lsort $epochs] != [lsort -unique $epochs]} {
                set ok 0 ; # At least one collision!
            }
        }
        if {$ok} break
        after 1000
        puts -nonewline .
        flush stdout
    }
    if {$trynum == 0} {
        fail "Config epoch conflict resolution is not working."
    }
}

test "Nodes should report cluster_state is ok now" {
    assert_cluster_state ok
}

test "Sanity for CLUSTER COUNTKEYSINSLOT" {
    set reply [R 0 CLUSTER COUNTKEYSINSLOT 0]
    assert {$reply eq 0}
}

test "It is possible to write and read from the cluster" {
    cluster_write_test 0
}

test "Native bitmap BITOP works in cluster hash slots" {
    set slot [R 0 cluster keyslot "{bitop}foo"]
    set port [get_instance_attrib redis 0 port]
    set cluster [redis_cluster 127.0.0.1:$port]
    array set node [$cluster masternode_for_slot $slot]
    set owner $node(link)

    assert_equal 0 [$owner setbit "{bitop}foo" 1 1]
    assert_equal 0 [$owner setbit "{bitop}bar" 2 1]
    assert_equal OK [$owner bitmap convert "{bitop}foo"]
    assert_equal OK [$owner bitmap convert "{bitop}bar"]
    assert_equal bitmap [$owner type "{bitop}foo"]
    assert_equal bitmap-roaring [$owner object encoding "{bitop}foo"]

    assert_equal 1 [$owner bitop or "{bitop}dest" "{bitop}foo" "{bitop}bar"]
    assert_equal bitmap [$owner type "{bitop}dest"]
    assert_equal bitmap-roaring [$owner object encoding "{bitop}dest"]
    assert_equal 1 [$owner getbit "{bitop}dest" 1]
    assert_equal 1 [$owner getbit "{bitop}dest" 2]

    assert_equal 1 [$owner bitop not "{bitop}not" "{bitop}foo"]
    assert_equal bitmap [$owner type "{bitop}not"]
    assert_equal bitmap-roaring [$owner object encoding "{bitop}not"]
    assert_equal 1 [$owner getbit "{bitop}not" 0]
    assert_equal 0 [$owner getbit "{bitop}not" 1]
    assert_equal 1 [$owner getbit "{bitop}foo" 1]
    assert_equal PONG [$owner ping]

    $cluster close
}

test "CLUSTER RESET SOFT test" {
    set last_epoch_node0 [get_info_field [R 0 cluster info] cluster_current_epoch]
    R 0 FLUSHALL
    R 0 CLUSTER RESET
    assert {[get_info_field [R 0 cluster info] cluster_current_epoch] eq $last_epoch_node0}

    set last_epoch_node1 [get_info_field [R 1 cluster info] cluster_current_epoch]
    R 1 FLUSHALL
    R 1 CLUSTER RESET SOFT
    assert {[get_info_field [R 1 cluster info] cluster_current_epoch] eq $last_epoch_node1}
}

test "Coverage: CLUSTER HELP" {
    assert_match "*CLUSTER <subcommand> *" [R 0 CLUSTER HELP]
}

test "Coverage: ASKING" {
    assert_equal {OK} [R 0 ASKING]
}

test "CLUSTER SLAVES and CLUSTER REPLICAS with zero replicas" {
    assert_equal {} [R 0 cluster slaves [R 0 CLUSTER MYID]]
    assert_equal {} [R 0 cluster replicas [R 0 CLUSTER MYID]]
}

test "CLUSTER FORGET with invalid node ID" {
    assert_error {*ERR Unknown node*} {R 0 cluster forget 1}
}
